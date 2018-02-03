#include "clang_complete.h"
#include "code_complete_cache.h"
#include "include_complete.h"
#include "message_handler.h"
#include "queue_manager.h"
#include "timer.h"
#include "working_files.h"

#include "lex_utils.h"

#include <loguru.hpp>

namespace {

// How a completion was triggered
enum class lsCompletionTriggerKind {
  // Completion was triggered by typing an identifier (24x7 code
  // complete), manual invocation (e.g Ctrl+Space) or via API.
  Invoked = 1,

  // Completion was triggered by a trigger character specified by
  // the `triggerCharacters` properties of the `CompletionRegistrationOptions`.
  TriggerCharacter = 2
};
MAKE_REFLECT_TYPE_PROXY(lsCompletionTriggerKind);

// Contains additional information about the context in which a completion
// request is triggered.
struct lsCompletionContext {
  // How the completion was triggered.
  lsCompletionTriggerKind triggerKind;

  // The trigger character (a single character) that has trigger code complete.
  // Is undefined if `triggerKind !== CompletionTriggerKind.TriggerCharacter`
  optional<std::string> triggerCharacter;
};
MAKE_REFLECT_STRUCT(lsCompletionContext, triggerKind, triggerCharacter);

struct lsCompletionParams : lsTextDocumentPositionParams {
  // The completion context. This is only available it the client specifies to
  // send this using
  // `ClientCapabilities.textDocument.completion.contextSupport === true`
  optional<lsCompletionContext> context;
};
MAKE_REFLECT_STRUCT(lsCompletionParams, textDocument, position, context);

struct Ipc_TextDocumentComplete
    : public RequestMessage<Ipc_TextDocumentComplete> {
  const static IpcId kIpcId = IpcId::TextDocumentCompletion;
  lsCompletionParams params;
};
MAKE_REFLECT_STRUCT(Ipc_TextDocumentComplete, id, params);
REGISTER_IPC_MESSAGE(Ipc_TextDocumentComplete);

struct lsTextDocumentCompleteResult {
  // This list it not complete. Further typing should result in recomputing
  // this list.
  bool isIncomplete = false;
  // The completion items.
  std::vector<lsCompletionItem> items;
};
MAKE_REFLECT_STRUCT(lsTextDocumentCompleteResult, isIncomplete, items);

struct Out_TextDocumentComplete
    : public lsOutMessage<Out_TextDocumentComplete> {
  lsRequestId id;
  lsTextDocumentCompleteResult result;
};
MAKE_REFLECT_STRUCT(Out_TextDocumentComplete, jsonrpc, id, result);

bool CompareLsCompletionItem(const lsCompletionItem& item1,
                             const lsCompletionItem& item2) {
  if (item1.found_ != item2.found_)
    return item1.found_ > item2.found_;
  if (item1.skip_ != item2.skip_)
    return item1.skip_ < item2.skip_;
  if (item1.priority_ != item2.priority_)
    return item1.priority_ < item2.priority_;
  if (item1.label.length() != item2.label.length())
    return item1.label.length() < item2.label.length();
  return item1.label < item2.label;
}

template <typename T>
char* tofixedbase64(T input, char* out) {
  const char* digits =
      "./0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  int len = (sizeof(T) * 8 - 1) / 6 + 1;
  for (int i = len - 1; i >= 0; i--) {
    out[i] = digits[input % 64];
    input /= 64;
  }
  out[len] = '\0';
  return out;
}

// Pre-filters completion responses before sending to vscode. This results in a
// significantly snappier completion experience as vscode is easily overloaded
// when given 1000+ completion items.
void FilterAndSortCompletionResponse(
    Out_TextDocumentComplete* complete_response,
    const std::string& complete_text,
    bool enable) {
  ScopedPerfTimer timer("FilterAndSortCompletionResponse");

// Used to inject more completions.
#if false
  const size_t kNumIterations = 250;
  size_t size = complete_response->result.items.size();
  complete_response->result.items.reserve(size * (kNumIterations + 1));
  for (size_t iteration = 0; iteration < kNumIterations; ++iteration) {
    for (size_t i = 0; i < size; ++i) {
      auto item = complete_response->result.items[i];
      item.label += "#" + std::to_string(iteration);
      complete_response->result.items.push_back(item);
    }
  }
#endif

  auto& items = complete_response->result.items;

  if (!enable) {
    // Just set the |sortText| to be the priority and return.
    char buf[16];
    for (auto& item : items)
      item.sortText = tofixedbase64(item.priority_, buf);
    return;
  }

  // If the text doesn't start with underscore,
  // remove all candidates that start with underscore.
  if (!complete_text.empty() && complete_text[0] != '_') {
    auto filter = [](const lsCompletionItem& item) {
      return item.label[0] == '_';
    };
    items.erase(std::remove_if(items.begin(), items.end(), filter),
                items.end());
  }

  // Fuzzy match.
  for (auto& item : items)
    std::tie(item.found_, item.skip_) =
        SubsequenceCountSkip(complete_text, item.label);

  // Order all items and set |sortText|.
  std::sort(items.begin(), items.end(), CompareLsCompletionItem);
  char buf[16];
  for (size_t i = 0; i < items.size(); ++i)
    items[i].sortText = tofixedbase64(i, buf);

  // If there are too many results...
  const size_t kMaxResultSize = 100u;
  if (items.size() > kMaxResultSize) {
    if (complete_text.empty()) {
      items.resize(kMaxResultSize);
    }
  }
}

struct TextDocumentCompletionHandler : MessageHandler {
  IpcId GetId() const override { return IpcId::TextDocumentCompletion; }

  void Run(std::unique_ptr<BaseIpcMessage> message) override {
    auto request = std::shared_ptr<Ipc_TextDocumentComplete>(
        static_cast<Ipc_TextDocumentComplete*>(message.release()));

    auto write_empty_result = [request]() {
      Out_TextDocumentComplete out;
      out.id = request->id;
      QueueManager::WriteStdout(IpcId::TextDocumentCompletion, out);
    };

    std::string path = request->params.textDocument.uri.GetPath();
    WorkingFile* file = working_files->GetFileByFilename(path);
    if (!file) {
      write_empty_result();
      return;
    }

    // It shouldn't be possible, but sometimes vscode will send queries out
    // of order, ie, we get completion request before buffer content update.
    std::string buffer_line;
    if (request->params.position.line >= 0 &&
        request->params.position.line < file->buffer_lines.size()) {
      buffer_line = file->buffer_lines[request->params.position.line];
    }

    // Check for - and : before completing -> or ::, since vscode does not
    // support multi-character trigger characters.
    if (request->params.context &&
        request->params.context->triggerKind ==
            lsCompletionTriggerKind::TriggerCharacter &&
        request->params.context->triggerCharacter) {
      bool did_fail_check = false;

      std::string character = *request->params.context->triggerCharacter;
      char preceding_index = request->params.position.character - 2;

      // If the character is '"' or '<', make sure the line is start with '#'.
      if (character == "\"" || character == "<") {
        size_t i = 0;
        while (i < buffer_line.size() && isspace(buffer_line[i]))
          ++i;
        if (i >= buffer_line.size() || buffer_line[i] != '#')
          did_fail_check = true;
      }
      // If the character is > or : and we are at the start of the line, do not
      // show completion results.
      else if ((character == ">" || character == ":") && preceding_index < 0) {
        did_fail_check = true;
      }
      // If the character is > but - does not preced it, or if it is : and :
      // does not preced it, do not show completion results.
      else if (preceding_index < buffer_line.size()) {
        char preceding = buffer_line[preceding_index];
        did_fail_check = (preceding != '-' && character == ">") ||
                         (preceding != ':' && character == ":");
      }

      if (did_fail_check) {
        write_empty_result();
        return;
      }
    }

    bool is_global_completion = false;
    std::string existing_completion;
    if (file) {
      request->params.position = file->FindStableCompletionSource(
          request->params.position, &is_global_completion,
          &existing_completion);
    }

    bool yes;
    std::string surround, prefix;
    std::tie(yes, surround, prefix) = ShouldRunIncludeCompletion(buffer_line);

    if (yes) {
      Out_TextDocumentComplete out;
      out.id = request->id;

      {
        std::unique_lock<std::mutex> lock(
            include_complete->completion_items_mutex, std::defer_lock);
        if (include_complete->is_scanning)
          lock.lock();
        out.result.items.assign(include_complete->completion_items.begin(),
                                include_complete->completion_items.end());
        if (lock)
          lock.unlock();
      }

      FilterAndSortCompletionResponse(&out, prefix,
                                      config->completion.filterAndSort);

      auto decorator = [&](std::string& text) {
        std::string result = "#include ";
        result += surround[0] + text + surround[1];
        text = result;
      };
      LOG_S(INFO) << "DEBUG prefix " << prefix;
      for (lsCompletionItem& item : out.result.items) {
        item.textEdit->range.start.line = request->params.position.line;
        item.textEdit->range.start.character = 0;
        item.textEdit->range.end.line = request->params.position.line;
        item.textEdit->range.end.character = (int)buffer_line.size();
        decorator(item.textEdit->newText);
        decorator(item.label);
      }

      QueueManager::WriteStdout(IpcId::TextDocumentCompletion, out);
    } else {
      // If existing completion is empty, dont return clang-based completion
      // results Only do this when trigger is not manual or context doesn't
      // exist (for Atom support).
      if (existing_completion.empty() && is_global_completion &&
          (request->params.context && request->params.context->triggerKind !=
                                          lsCompletionTriggerKind::Invoked)) {
        LOG_S(INFO) << "Existing completion is empty, no completion results "
                       "will be returned";
        Out_TextDocumentComplete out;
        out.id = request->id;
        QueueManager::WriteStdout(IpcId::TextDocumentCompletion, out);
        return;
      }

      ClangCompleteManager::OnComplete callback = std::bind(
          [this, is_global_completion, existing_completion, request](
              const std::vector<lsCompletionItem>& results,
              bool is_cached_result) {
            Out_TextDocumentComplete out;
            out.id = request->id;
            out.result.items = results;

            // Emit completion results.
            FilterAndSortCompletionResponse(&out, existing_completion,
                                            config->completion.filterAndSort);
            QueueManager::WriteStdout(IpcId::TextDocumentCompletion, out);

            // Cache completion results.
            if (!is_cached_result) {
              std::string path = request->params.textDocument.uri.GetPath();
              if (is_global_completion) {
                global_code_complete_cache->WithLock([&]() {
                  global_code_complete_cache->cached_path_ = path;
                  global_code_complete_cache->cached_results_ = results;
                });
              } else {
                non_global_code_complete_cache->WithLock([&]() {
                  non_global_code_complete_cache->cached_path_ = path;
                  non_global_code_complete_cache->cached_completion_position_ =
                      request->params.position;
                  non_global_code_complete_cache->cached_results_ = results;
                });
              }
            }
          },
          std::placeholders::_1, std::placeholders::_2);

      bool is_cache_match = false;
      global_code_complete_cache->WithLock([&]() {
        is_cache_match = is_global_completion &&
                         global_code_complete_cache->cached_path_ == path &&
                         !global_code_complete_cache->cached_results_.empty();
      });
      if (is_cache_match) {
        ClangCompleteManager::OnComplete freshen_global =
            [this](std::vector<lsCompletionItem> results,
                   bool is_cached_result) {
              assert(!is_cached_result);

              // note: path is updated in the normal completion handler.
              global_code_complete_cache->WithLock([&]() {
                global_code_complete_cache->cached_results_ = results;
              });
            };

        global_code_complete_cache->WithLock([&]() {
          callback(global_code_complete_cache->cached_results_,
                   true /*is_cached_result*/);
        });
        clang_complete->CodeComplete(request->params, freshen_global);
      } else if (non_global_code_complete_cache->IsCacheValid(
                     request->params)) {
        non_global_code_complete_cache->WithLock([&]() {
          callback(non_global_code_complete_cache->cached_results_,
                   true /*is_cached_result*/);
        });
      } else {
        clang_complete->CodeComplete(request->params, callback);
      }
    }
  }
};
REGISTER_MESSAGE_HANDLER(TextDocumentCompletionHandler);

}  // namespace
