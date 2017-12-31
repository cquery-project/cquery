#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {

lsWorkspaceEdit BuildWorkspaceEdit(QueryDatabase* db,
                                   WorkingFiles* working_files,
                                   const std::vector<QueryLocation>& locations,
                                   const std::string& new_text) {
  std::unordered_map<QueryFileId, lsTextDocumentEdit> path_to_edit;

  for (auto& location : locations) {
    optional<lsLocation> ls_location =
        GetLsLocation(db, working_files, location);
    if (!ls_location)
      continue;

    if (path_to_edit.find(location.path) == path_to_edit.end()) {
      path_to_edit[location.path] = lsTextDocumentEdit();

      QueryFile& file = db->files[location.path.id];
      if (!file.def)
        continue;

      const std::string& path = file.def->path;
      path_to_edit[location.path].textDocument.uri =
          lsDocumentUri::FromPath(path);

      WorkingFile* working_file = working_files->GetFileByFilename(path);
      if (working_file)
        path_to_edit[location.path].textDocument.version =
            working_file->version;
    }

    lsTextEdit edit;
    edit.range = ls_location->range;
    edit.newText = new_text;

    // vscode complains if we submit overlapping text edits.
    auto& edits = path_to_edit[location.path].edits;
    if (std::find(edits.begin(), edits.end(), edit) == edits.end())
      edits.push_back(edit);
  }

  lsWorkspaceEdit edit;
  for (const auto& changes : path_to_edit)
    edit.documentChanges.push_back(changes.second);
  return edit;
}

struct Ipc_TextDocumentRename : public IpcMessage<Ipc_TextDocumentRename> {
  struct Params {
    // The document to format.
    lsTextDocumentIdentifier textDocument;

    // The position at which this request was sent.
    lsPosition position;

    // The new name of the symbol. If the given name is not valid the
    // request must return a [ResponseError](#ResponseError) with an
    // appropriate message set.
    std::string newName;
  };
  const static IpcId kIpcId = IpcId::TextDocumentRename;

  lsRequestId id;
  Params params;
};
MAKE_REFLECT_STRUCT(Ipc_TextDocumentRename::Params,
                    textDocument,
                    position,
                    newName);
MAKE_REFLECT_STRUCT(Ipc_TextDocumentRename, id, params);
REGISTER_IPC_MESSAGE(Ipc_TextDocumentRename);

struct Out_TextDocumentRename : public lsOutMessage<Out_TextDocumentRename> {
  lsRequestId id;
  lsWorkspaceEdit result;
};
MAKE_REFLECT_STRUCT(Out_TextDocumentRename, jsonrpc, id, result);

struct TextDocumentRenameHandler : BaseMessageHandler<Ipc_TextDocumentRename> {
  void Run(Ipc_TextDocumentRename* request) override {
    QueryFileId file_id;
    QueryFile* file;
    if (!FindFileOrFail(db, project, request->id,
                        request->params.textDocument.uri.GetPath(), &file,
                        &file_id)) {
      return;
    }

    WorkingFile* working_file =
        working_files->GetFileByFilename(file->def->path);

    Out_TextDocumentRename out;
    out.id = request->id;

    for (const SymbolRef& ref :
         FindSymbolsAtLocation(working_file, file, request->params.position)) {
      // Found symbol. Return references to rename.
      std::vector<QueryLocation> uses = GetUsesOfSymbol(db, ref.idx);
      out.result =
          BuildWorkspaceEdit(db, working_files, uses, request->params.newName);
      break;
    }

    QueueManager::WriteStdout(IpcId::TextDocumentRename, out);
  }
};
REGISTER_MESSAGE_HANDLER(TextDocumentRenameHandler);
}  // namespace
