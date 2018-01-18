#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
struct Ipc_CqueryMemberHierarchyInitial
    : public IpcMessage<Ipc_CqueryMemberHierarchyInitial> {
  const static IpcId kIpcId = IpcId::CqueryMemberHierarchyInitial;
  lsRequestId id;
  lsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(Ipc_CqueryMemberHierarchyInitial, id, params);
REGISTER_IPC_MESSAGE(Ipc_CqueryMemberHierarchyInitial);

struct Ipc_CqueryMemberHierarchyExpand
    : public IpcMessage<Ipc_CqueryMemberHierarchyExpand> {
  const static IpcId kIpcId = IpcId::CqueryMemberHierarchyExpand;
  lsRequestId id;
  struct Params {
    size_t type_id;
  };
  Params params;
};
MAKE_REFLECT_STRUCT(Ipc_CqueryMemberHierarchyExpand::Params, type_id);
MAKE_REFLECT_STRUCT(Ipc_CqueryMemberHierarchyExpand, id, params);
REGISTER_IPC_MESSAGE(Ipc_CqueryMemberHierarchyExpand);

struct Out_CqueryMemberHierarchy
    : public lsOutMessage<Out_CqueryMemberHierarchy> {
  struct Entry {
    std::string name;
    size_t type_id;
    lsLocation location;
  };
  lsRequestId id;
  std::vector<Entry> result;
};
MAKE_REFLECT_STRUCT(Out_CqueryMemberHierarchy::Entry,
                    name,
                    type_id,
                    location);
MAKE_REFLECT_STRUCT(Out_CqueryMemberHierarchy, jsonrpc, id, result);

std::vector<Out_CqueryMemberHierarchy::Entry> BuildInitial(
    QueryDatabase* db,
    WorkingFiles* working_files,
    QueryTypeId root) {
  QueryType& root_type = db->types[root.id];
  if (!root_type.def || !root_type.def->definition_spelling)
    return {};
  optional<lsLocation> def_loc =
      GetLsLocation(db, working_files, *root_type.def->definition_spelling);
  if (!def_loc)
    return {};

  Out_CqueryMemberHierarchy::Entry entry;
  entry.name = root_type.def->short_name;
  entry.type_id = root.id;
  entry.location = *def_loc;
  return {entry};
}

std::vector<Out_CqueryMemberHierarchy::Entry>
ExpandNode(QueryDatabase* db,
           WorkingFiles* working_files,
           QueryTypeId root) {
  QueryType& root_type = db->types[root.id];
  if (!root_type.def)
    return {};

  std::vector<Out_CqueryMemberHierarchy::Entry> ret;
  for (auto& var_id : root_type.def->vars) {
    QueryVar& var = db->vars[var_id.id];
    Out_CqueryMemberHierarchy::Entry entry;
    entry.name = var.def->short_name;
    entry.type_id =
        var.def->variable_type ? var.def->variable_type->id : size_t(-1);
    if (var.def->definition_spelling) {
      optional<lsLocation> loc =
          GetLsLocation(db, working_files, *var.def->definition_spelling);
      // TODO invalid location
      if (loc)
        entry.location = *loc;
    }
    ret.push_back(std::move(entry));
  }
  return ret;
}

struct CqueryMemberHierarchyInitialHandler
    : BaseMessageHandler<Ipc_CqueryMemberHierarchyInitial> {
  void Run(Ipc_CqueryMemberHierarchyInitial* request) override {
    QueryFile* file;
    if (!FindFileOrFail(db, project, request->id,
                        request->params.textDocument.uri.GetPath(), &file))
      return;

    WorkingFile* working_file =
        working_files->GetFileByFilename(file->def->path);
    Out_CqueryMemberHierarchy out;
    out.id = request->id;

    for (const SymbolRef& ref :
             FindSymbolsAtLocation(working_file, file, request->params.position)) {
      if (ref.idx.kind == SymbolKind::Type) {
        out.result = BuildInitial(db, working_files, QueryTypeId(ref.idx.idx));
        break;
      }
      if (ref.idx.kind == SymbolKind::Var) {
        QueryVar& var = db->vars[ref.idx.idx];
        if (var.def && var.def->variable_type)
          out.result = BuildInitial(db, working_files, *var.def->variable_type);
        break;
      }
    }

    QueueManager::WriteStdout(IpcId::CqueryMemberHierarchyInitial, out);
  }
};
REGISTER_MESSAGE_HANDLER(CqueryMemberHierarchyInitialHandler);

struct CqueryMemberHierarchyExpandHandler
    : BaseMessageHandler<Ipc_CqueryMemberHierarchyExpand> {
  void Run(Ipc_CqueryMemberHierarchyExpand* request) override {
    Out_CqueryMemberHierarchy out;
    out.id = request->id;
    // |ExpandNode| uses -1 to indicate invalid |type_id|.
    if (request->params.type_id != size_t(-1))
      out.result =
          ExpandNode(db, working_files, QueryTypeId(request->params.type_id));

    QueueManager::WriteStdout(IpcId::CqueryMemberHierarchyExpand, out);
  }
};
REGISTER_MESSAGE_HANDLER(CqueryMemberHierarchyExpandHandler);
}  // namespace
