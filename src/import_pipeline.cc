#include "import_pipeline.h"

#include "cache_manager.h"
#include "config.h"
#include "iindexer.h"
#include "import_manager.h"
#include "language_server_api.h"
#include "message_handler.h"
#include "platform.h"
#include "project.h"
#include "query_utils.h"
#include "queue_manager.h"
#include "timer.h"
#include "timestamp_manager.h"

#include <doctest/doctest.h>
#include <loguru.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

long long GetCurrentTimeInMilliseconds() {
  auto time_since_epoch = Timer::Clock::now().time_since_epoch();
  long long elapsed_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(time_since_epoch)
          .count();
  return elapsed_milliseconds;
}

struct ActiveThread {
  ActiveThread(Config* config, ImportPipelineStatus* status)
      : config_(config), status_(status) {
    if (config_->progressReportFrequencyMs < 0)
      return;

    ++status_->num_active_threads;
  }
  ~ActiveThread() {
    if (config_->progressReportFrequencyMs < 0)
      return;

    --status_->num_active_threads;
    EmitProgress();
  }

  // Send indexing progress to client if reporting is enabled.
  void EmitProgress() {
    auto* queue = QueueManager::instance();
    Out_Progress out;
    out.params.indexRequestCount = queue->index_request.Size();
    out.params.doIdMapCount = queue->do_id_map.Size();
    out.params.loadPreviousIndexCount = queue->load_previous_index.Size();
    out.params.onIdMappedCount = queue->on_id_mapped.Size();
    out.params.onIndexedCount = queue->on_indexed.Size();
    out.params.activeThreads = status_->num_active_threads;

    // Ignore this progress update if the last update was too recent.
    if (config_->progressReportFrequencyMs != 0) {
      // Make sure we output a status update if queue lengths are zero.
      bool all_zero =
          out.params.indexRequestCount == 0 && out.params.doIdMapCount == 0 &&
          out.params.loadPreviousIndexCount == 0 &&
          out.params.onIdMappedCount == 0 && out.params.onIndexedCount == 0 &&
          out.params.activeThreads == 0;
      if (!all_zero ||
          GetCurrentTimeInMilliseconds() < status_->next_progress_output)
        return;
      status_->next_progress_output =
          GetCurrentTimeInMilliseconds() + config_->progressReportFrequencyMs;
    }

    QueueManager::WriteStdout(IpcId::Unknown, out);
  }

  Config* config_;
  ImportPipelineStatus* status_;
};

enum class FileParseQuery { NeedsParse, DoesNotNeedParse, NoSuchFile };

std::vector<Index_DoIdMap> DoParseFile(
    Config* config,
    WorkingFiles* working_files,
    FileConsumerSharedState* file_consumer_shared,
    TimestampManager* timestamp_manager,
    ImportManager* import_manager,
    ICacheManager* cache_manager,
    IIndexer* indexer,
    bool is_interactive,
    const std::string& path,
    const std::vector<std::string>& args,
    const FileContents& contents) {
  std::vector<Index_DoIdMap> result;

  // Always run this block, even if we are interactive, so we can check
  // dependencies and reset files in |file_consumer_shared|.
  IndexFile* previous_index = cache_manager->TryLoad(path);
  if (previous_index) {
    // If none of the dependencies have changed and the index is not
    // interactive (ie, requested by a file save), skip parsing and just load
    // from cache.

    // Checks if |path| needs to be reparsed. This will modify cached state
    // such that calling this function twice with the same path may return true
    // the first time but will return false the second.
    auto file_needs_parse = [&](const std::string& path, bool is_dependency) {
      // If the file is a dependency but another file as already imported it,
      // don't bother.
      if (!is_interactive && is_dependency &&
          !import_manager->TryMarkDependencyImported(path)) {
        return FileParseQuery::DoesNotNeedParse;
      }

      optional<int64_t> modification_timestamp = GetLastModificationTime(path);

      // Cannot find file.
      if (!modification_timestamp)
        return FileParseQuery::NoSuchFile;

      optional<int64_t> last_cached_modification =
          timestamp_manager->GetLastCachedModificationTime(cache_manager, path);

      // File has been changed.
      if (!last_cached_modification ||
          modification_timestamp != *last_cached_modification) {
        file_consumer_shared->Reset(path);
        return FileParseQuery::NeedsParse;
      }

      // File has not changed, do not parse it.
      return FileParseQuery::DoesNotNeedParse;
    };

    // Check timestamps and update |file_consumer_shared|.
    FileParseQuery path_state = file_needs_parse(path, false /*is_dependency*/);

    // Target file does not exist on disk, do not emit any indexes.
    // TODO: Dependencies should be reassigned to other files. We can do this by
    // updating the "primary_file" if it doesn't exist. Might not actually be a
    // problem in practice.
    if (path_state == FileParseQuery::NoSuchFile)
      return result;

    bool needs_reparse =
        is_interactive || path_state == FileParseQuery::NeedsParse;

    for (const std::string& dependency : previous_index->dependencies) {
      assert(!dependency.empty());

      // note: Use != as there are multiple failure results for FileParseQuery.
      if (file_needs_parse(dependency, true /*is_dependency*/) !=
          FileParseQuery::DoesNotNeedParse) {
        LOG_S(INFO) << "Timestamp has changed for " << dependency << " (via "
                    << previous_index->path << ")";
        needs_reparse = true;
        // SUBTLE: Do not break here, as |file_consumer_shared| is updated
        // inside of |file_needs_parse|.
      }
    }

    // No timestamps changed - load directly from cache.
    if (!needs_reparse) {
      LOG_S(INFO) << "Skipping parse; no timestamp change for " << path;

      // TODO/FIXME: real perf
      PerformanceImportFile perf;
      result.push_back(Index_DoIdMap(cache_manager->TakeOrLoad(path), perf,
                                     is_interactive, false /*write_to_disk*/));
      for (const std::string& dependency : previous_index->dependencies) {
        // Only load a dependency if it is not already loaded.
        //
        // This is important for perf in large projects where there are lots of
        // dependencies shared between many files.
        if (!file_consumer_shared->Mark(dependency))
          continue;

        LOG_S(INFO) << "Emitting index result for " << dependency << " (via "
                    << previous_index->path << ")";

        std::unique_ptr<IndexFile> dependency_index =
            cache_manager->TryTakeOrLoad(dependency);

        // |dependency_index| may be null if there is no cache for it but
        // another file has already started importing it.
        if (!dependency_index)
          continue;

        result.push_back(Index_DoIdMap(std::move(dependency_index), perf,
                                       is_interactive,
                                       false /*write_to_disk*/));
      }
      return result;
    }
  }

  LOG_S(INFO) << "Parsing " << path;

  // Load file contents for all dependencies into memory. If the dependencies
  // for the file changed we may not end up using all of the files we
  // preloaded. If a new dependency was added the indexer will grab the file
  // contents as soon as possible.
  //
  // We do this to minimize the race between indexing a file and capturing the
  // file contents.
  //
  // TODO: We might be able to optimize perf by only copying for files in
  //       working_files. We can pass that same set of files to the indexer as
  //       well. We then default to a fast file-copy if not in working set.
  bool loaded_primary = contents.path == path;
  std::vector<FileContents> file_contents = {contents};
  cache_manager->IterateLoadedCaches([&](IndexFile* index) {
    // FIXME: ReadContent should go through |cache_manager|.
    optional<std::string> index_content = ReadContent(index->path);
    if (!index_content) {
      LOG_S(ERROR) << "Failed to load index content for " << index->path;
      return;
    }

    file_contents.push_back(FileContents(index->path, *index_content));

    loaded_primary = loaded_primary || index->path == path;
  });
  if (!loaded_primary) {
    optional<std::string> content = ReadContent(path);
    if (!content) {
      LOG_S(ERROR) << "Skipping index (file cannot be found): " << path;
      return result;
    }
    file_contents.push_back(FileContents(path, *content));
  }

  PerformanceImportFile perf;
  std::vector<std::unique_ptr<IndexFile>> indexes = indexer->Index(
      config, file_consumer_shared, path, args, file_contents, &perf);
  for (std::unique_ptr<IndexFile>& new_index : indexes) {
    Timer time;

    // Only emit diagnostics for non-interactive sessions, which makes it easier
    // to identify indexing problems. For interactive sessions, diagnostics are
    // handled by code completion.
    if (!is_interactive)
      EmitDiagnostics(working_files, new_index->path, new_index->diagnostics_);

    // When main thread does IdMap request it will request the previous index if
    // needed.
    LOG_S(INFO) << "Emitting index result for " << new_index->path;
    result.push_back(Index_DoIdMap(std::move(new_index), perf, is_interactive,
                                   true /*write_to_disk*/));
  }

  return result;
}

std::vector<Index_DoIdMap> ParseFile(
    Config* config,
    WorkingFiles* working_files,
    FileConsumerSharedState* file_consumer_shared,
    TimestampManager* timestamp_manager,
    ImportManager* import_manager,
    ICacheManager* cache_manager,
    IIndexer* indexer,
    bool is_interactive,
    const Project::Entry& entry,
    const std::string& contents) {
  FileContents file_contents(entry.filename, contents);

  // Try to determine the original import file by loading the file from cache.
  // This lets the user request an index on a header file, which clang will
  // complain about if indexed by itself.
  IndexFile* entry_cache = cache_manager->TryLoad(entry.filename);
  std::string tu_path = entry_cache ? entry_cache->import_file : entry.filename;
  return DoParseFile(config, working_files, file_consumer_shared,
                     timestamp_manager, import_manager, cache_manager, indexer,
                     is_interactive, tu_path, entry.args, file_contents);
}

bool IndexMain_DoParse(Config* config,
                       WorkingFiles* working_files,
                       FileConsumerSharedState* file_consumer_shared,
                       TimestampManager* timestamp_manager,
                       ImportManager* import_manager,
                       ICacheManager* cache_manager,
                       IIndexer* indexer) {
  auto* queue = QueueManager::instance();
  optional<Index_Request> request = queue->index_request.TryDequeue();
  if (!request)
    return false;

  Project::Entry entry;
  entry.filename = request->path;
  entry.args = request->args;
  std::vector<Index_DoIdMap> responses =
      ParseFile(config, working_files, file_consumer_shared, timestamp_manager,
                import_manager, cache_manager, indexer, request->is_interactive,
                entry, request->contents);

  // Don't bother sending an IdMap request if there are no responses. This
  // avoids a lock.
  if (responses.empty())
    return false;

  // EnqueueAll will clear |responses|.
  queue->do_id_map.EnqueueAll(std::move(responses));
  return true;
}

bool IndexMain_DoCreateIndexUpdate(TimestampManager* timestamp_manager,
                                   ICacheManager* cache_manager) {
  auto* queue = QueueManager::instance();
  optional<Index_OnIdMapped> response = queue->on_id_mapped.TryDequeue();
  if (!response)
    return false;

  Timer time;

  IdMap* previous_id_map = nullptr;
  IndexFile* previous_index = nullptr;
  if (response->previous) {
    previous_id_map = response->previous->ids.get();
    previous_index = response->previous->file.get();
  }

  // Build delta update.
  IndexUpdate update =
      IndexUpdate::CreateDelta(previous_id_map, response->current->ids.get(),
                               previous_index, response->current->file.get());
  response->perf.index_make_delta = time.ElapsedMicrosecondsAndReset();
  LOG_S(INFO) << "Built index update for " << response->current->file->path
              << " (is_delta=" << !!response->previous << ")";

  // Write current index to disk if requested.
  if (response->write_to_disk) {
    LOG_S(INFO) << "Writing cached index to disk for "
                << response->current->file->path;
    time.Reset();
    cache_manager->WriteToCache(*response->current->file);
    response->perf.index_save_to_disk = time.ElapsedMicrosecondsAndReset();
    timestamp_manager->UpdateCachedModificationTime(
        response->current->file->path,
        response->current->file->last_modification_time);
  }

#if false
#define PRINT_SECTION(name)                                                    \
  if (response->perf.name) {                                                   \
    total += response->perf.name;                                              \
    output << " " << #name << ": " << FormatMicroseconds(response->perf.name); \
  }
  std::stringstream output;
  long long total = 0;
  output << "[perf]";
  PRINT_SECTION(index_parse);
  PRINT_SECTION(index_build);
  PRINT_SECTION(index_save_to_disk);
  PRINT_SECTION(index_load_cached);
  PRINT_SECTION(querydb_id_map);
  PRINT_SECTION(index_make_delta);
  output << "\n       total: " << FormatMicroseconds(total);
  output << " path: " << response->current_index->path;
  LOG_S(INFO) << output.rdbuf();
#undef PRINT_SECTION

  if (response->is_interactive)
    LOG_S(INFO) << "Applying IndexUpdate" << std::endl << update.ToString();
#endif

  Index_OnIndexed reply(update, response->perf);
  queue->on_indexed.Enqueue(std::move(reply));

  return true;
}

bool IndexMain_LoadPreviousIndex(ICacheManager* cache_manager) {
  auto* queue = QueueManager::instance();
  optional<Index_DoIdMap> response = queue->load_previous_index.TryDequeue();
  if (!response)
    return false;

  response->previous = cache_manager->TryTakeOrLoad(response->current->path);
  LOG_IF_S(ERROR, !response->previous)
      << "Unable to load previous index for already imported index "
      << response->current->path;

  queue->do_id_map.Enqueue(std::move(*response));
  return true;
}

bool IndexMergeIndexUpdates() {
  auto* queue = QueueManager::instance();
  optional<Index_OnIndexed> root = queue->on_indexed.TryDequeue();
  if (!root)
    return false;

  bool did_merge = false;
  while (true) {
    optional<Index_OnIndexed> to_join = queue->on_indexed.TryDequeue();
    if (!to_join) {
      queue->on_indexed.Enqueue(std::move(*root));
      return did_merge;
    }

    did_merge = true;
    Timer time;
    root->update.Merge(to_join->update);
    // time.ResetAndPrint("Joined querydb updates for files: " +
    // StringJoinMap(root->update.files_def_update,
    //[](const QueryFile::DefUpdate& update) {
    // return update.path;
    //}));
  }
}

}  // namespace

ImportPipelineStatus::ImportPipelineStatus()
    : num_active_threads(0), next_progress_output(0) {}

// Index a file using an already-parsed translation unit from code completion.
// Since most of the time for indexing a file comes from parsing, we can do
// real-time indexing.
// TODO: add option to disable this.
void IndexWithTuFromCodeCompletion(
    FileConsumerSharedState* file_consumer_shared,
    ClangTranslationUnit* tu,
    const std::vector<CXUnsavedFile>& file_contents,
    const std::string& path,
    const std::vector<std::string>& args) {
  file_consumer_shared->Reset(path);

  PerformanceImportFile perf;
  ClangIndex index;
  std::vector<std::unique_ptr<IndexFile>> indexes = ParseWithTu(
      file_consumer_shared, &perf, tu, &index, path, args, file_contents);

  std::vector<Index_DoIdMap> result;
  for (std::unique_ptr<IndexFile>& new_index : indexes) {
    Timer time;

    // When main thread does IdMap request it will request the previous index if
    // needed.
    LOG_S(INFO) << "Emitting index result for " << new_index->path;
    result.push_back(Index_DoIdMap(std::move(new_index), perf,
                                   true /*is_interactive*/,
                                   true /*write_to_disk*/));
  }

  LOG_IF_S(WARNING, result.size() > 1)
      << "Code completion index update generated more than one index";

  QueueManager::instance()->do_id_map.EnqueueAll(std::move(result));
}

void Indexer_Main(Config* config,
                  FileConsumerSharedState* file_consumer_shared,
                  TimestampManager* timestamp_manager,
                  ImportManager* import_manager,
                  ImportPipelineStatus* status,
                  Project* project,
                  WorkingFiles* working_files,
                  MultiQueueWaiter* waiter) {
  std::unique_ptr<ICacheManager> cache_manager = ICacheManager::Make(config);
  auto* queue = QueueManager::instance();
  // Build one index per-indexer, as building the index acquires a global lock.
  auto indexer = IIndexer::MakeClangIndexer();

  while (true) {
    bool did_work = false;

    {
      ActiveThread active_thread(config, status);

      // TODO: process all off IndexMain_DoIndex before calling
      // IndexMain_DoCreateIndexUpdate for better icache behavior. We need to
      // have some threads spinning on both though otherwise memory usage will
      // get bad.

      // We need to make sure to run both IndexMain_DoParse and
      // IndexMain_DoCreateIndexUpdate so we don't starve querydb from doing any
      // work. Running both also lets the user query the partially constructed
      // index.
      std::unique_ptr<ICacheManager> cache_manager =
          ICacheManager::Make(config);
      did_work = IndexMain_DoParse(config, working_files, file_consumer_shared,
                                   timestamp_manager, import_manager,
                                   cache_manager.get(), indexer.get()) ||
                 did_work;

      did_work = IndexMain_DoCreateIndexUpdate(timestamp_manager,
                                               cache_manager.get()) ||
                 did_work;

      did_work = IndexMain_LoadPreviousIndex(cache_manager.get()) || did_work;

      // Nothing to index and no index updates to create, so join some already
      // created index updates to reduce work on querydb thread.
      if (!did_work)
        did_work = IndexMergeIndexUpdates() || did_work;
    }

    // We didn't do any work, so wait for a notification.
    if (!did_work) {
      waiter->Wait(&queue->on_indexed, &queue->index_request,
                   &queue->on_id_mapped, &queue->load_previous_index);
    }
  }
}

bool QueryDb_ImportMain(Config* config,
                        QueryDatabase* db,
                        ImportManager* import_manager,
                        ImportPipelineStatus* status,
                        SemanticHighlightSymbolCache* semantic_cache,
                        WorkingFiles* working_files) {
  std::unique_ptr<ICacheManager> cache_manager = ICacheManager::Make(config);
  auto* queue = QueueManager::instance();

  ActiveThread active_thread(config, status);

  bool did_work = false;

  while (true) {
    optional<Index_DoIdMap> request = queue->do_id_map.TryDequeue();
    if (!request)
      break;
    did_work = true;

    assert(request->current);

    // If the request does not have previous state and we have already imported
    // it, load the previous state from disk and rerun IdMap logic later. Do not
    // do this if we have already attempted in the past.
    if (!request->load_previous && !request->previous &&
        db->usr_to_file.find(LowerPathIfCaseInsensitive(
            request->current->path)) != db->usr_to_file.end()) {
      assert(!request->load_previous);
      request->load_previous = true;
      queue->load_previous_index.Enqueue(std::move(*request));
      continue;
    }

    // Check if the file is already being imported into querydb. If it is, drop
    // the request.
    //
    // Note, we must do this *after* we have checked for the previous index,
    // otherwise we will never actually generate the IdMap.
    if (!import_manager->StartQueryDbImport(request->current->path)) {
      LOG_S(INFO) << "Dropping index as it is already being imported for "
                  << request->current->path;
      continue;
    }

    Index_OnIdMapped response(request->perf, request->is_interactive,
                              request->write_to_disk);
    Timer time;

    auto make_map = [db](std::unique_ptr<IndexFile> file)
        -> std::unique_ptr<Index_OnIdMapped::File> {
      if (!file)
        return nullptr;

      auto id_map = MakeUnique<IdMap>(db, file->id_cache);
      return MakeUnique<Index_OnIdMapped::File>(std::move(file),
                                                std::move(id_map));
    };
    response.current = make_map(std::move(request->current));
    response.previous = make_map(std::move(request->previous));
    response.perf.querydb_id_map = time.ElapsedMicrosecondsAndReset();

    queue->on_id_mapped.Enqueue(std::move(response));
  }

  while (true) {
    optional<Index_OnIndexed> response = queue->on_indexed.TryDequeue();
    if (!response)
      break;

    did_work = true;

    Timer time;

    for (auto& updated_file : response->update.files_def_update) {
      // TODO: We're reading a file on querydb thread. This is slow!! If this
      // a real problem in practice we can load the file in a previous stage.
      // It should be fine though because we only do it if the user has the
      // file open.
      WorkingFile* working_file =
          working_files->GetFileByFilename(updated_file.path);
      if (working_file) {
        optional<std::string> cached_file_contents =
            cache_manager->LoadCachedFileContents(updated_file.path);
        if (cached_file_contents)
          working_file->SetIndexContent(*cached_file_contents);
        else
          working_file->SetIndexContent(working_file->buffer_content);
        time.ResetAndPrint(
            "Update WorkingFile index contents (via disk load) for " +
            updated_file.path);

        // Update inactive region.
        EmitInactiveLines(working_file, updated_file.inactive_regions);
      }
    }

    time.Reset();
    db->ApplyIndexUpdate(&response->update);
    time.ResetAndPrint("Applying index update for " +
                       StringJoinMap(response->update.files_def_update,
                                     [](const QueryFile::DefUpdate& value) {
                                       return value.path;
                                     }));

    // Update semantic highlighting.
    for (auto& updated_file : response->update.files_def_update) {
      WorkingFile* working_file =
          working_files->GetFileByFilename(updated_file.path);
      if (working_file) {
        QueryFileId file_id = db->usr_to_file[LowerPathIfCaseInsensitive(working_file->filename)];
        QueryFile* file = &db->files[file_id.id];
        EmitSemanticHighlighting(db, semantic_cache, working_file, file);
      }
    }

    // Mark the files as being done in querydb stage after we apply the index
    // update.
    for (auto& updated_file : response->update.files_def_update)
      import_manager->DoneQueryDbImport(updated_file.path);
  }

  return did_work;
}

TEST_SUITE("ImportPipeline") {
  struct Fixture {
    Fixture() {
      QueueManager::CreateInstance(&querydb_waiter, &indexer_waiter,
                                   &stdout_waiter);

      queue = QueueManager::instance();
      cache_manager = ICacheManager::MakeFake({});
      indexer = IIndexer::MakeTestIndexer({});
    }

    bool PumpOnce() {
      return IndexMain_DoParse(&config, &working_files, &file_consumer_shared,
                               &timestamp_manager, &import_manager,
                               cache_manager.get(), indexer.get());
    }

    void MakeRequest(const std::string& path,
                     const std::vector<std::string>& args = {},
                     bool is_interactive = false,
                     const std::string& contents = "void foo();") {
      queue->index_request.Enqueue(
          Index_Request(path, args, is_interactive, contents));
    }

    MultiQueueWaiter querydb_waiter;
    MultiQueueWaiter indexer_waiter;
    MultiQueueWaiter stdout_waiter;

    QueueManager* queue = nullptr;
    Config config;
    WorkingFiles working_files;
    FileConsumerSharedState file_consumer_shared;
    TimestampManager timestamp_manager;
    ImportManager import_manager;
    std::unique_ptr<ICacheManager> cache_manager;
    std::unique_ptr<IIndexer> indexer;
  };

  // FIXME: validate other state like timestamp_manager, etc.
  // FIXME: add more interesting tests that are not the happy path
  // FIXME: test
  //   - IndexMain_DoCreateIndexUpdate
  //   - IndexMain_LoadPreviousIndex
  //   - QueryDb_ImportMain

  TEST_CASE_FIXTURE(Fixture, "index request with zero results") {
    indexer = IIndexer::MakeTestIndexer({IIndexer::TestEntry{"foo.cc", 0}});

    MakeRequest("foo.cc");

    REQUIRE(queue->index_request.Size() == 1);
    REQUIRE(queue->do_id_map.Size() == 0);
    PumpOnce();
    REQUIRE(queue->index_request.Size() == 0);
    REQUIRE(queue->do_id_map.Size() == 0);

    REQUIRE(file_consumer_shared.used_files.empty());
  }

  TEST_CASE_FIXTURE(Fixture, "one index request") {
    indexer = IIndexer::MakeTestIndexer({IIndexer::TestEntry{"foo.cc", 100}});

    MakeRequest("foo.cc");

    REQUIRE(queue->index_request.Size() == 1);
    REQUIRE(queue->do_id_map.Size() == 0);
    PumpOnce();
    REQUIRE(queue->index_request.Size() == 0);
    REQUIRE(queue->do_id_map.Size() == 100);

    REQUIRE(file_consumer_shared.used_files.empty());
  }

  TEST_CASE_FIXTURE(Fixture, "multiple index requests") {
    indexer = IIndexer::MakeTestIndexer(
        {IIndexer::TestEntry{"foo.cc", 100}, IIndexer::TestEntry{"bar.cc", 5}});

    MakeRequest("foo.cc");
    MakeRequest("bar.cc");

    REQUIRE(queue->index_request.Size() == 2);
    REQUIRE(queue->do_id_map.Size() == 0);
    while (PumpOnce()) {
    }
    REQUIRE(queue->index_request.Size() == 0);
    REQUIRE(queue->do_id_map.Size() == 105);

    REQUIRE(file_consumer_shared.used_files.empty());
  }
}
