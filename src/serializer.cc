#include "serializer.h"

#include "serializers/json.h"
#include "serializers/msgpack.h"

#include "indexer.h"

#include <doctest/doctest.h>
#include <loguru.hpp>

namespace {
bool gTestOutputMode = false;

std::string GetBaseName(const std::string& path) {
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos && (last_slash + 1) < path.size())
    return path.substr(last_slash + 1);
  return path;
}

}  // namespace

// uint8_t
void Reflect(Reader& visitor, uint8_t& value) {
  value = (uint8_t)visitor.GetInt();
}
void Reflect(Writer& visitor, uint8_t& value) {
  visitor.Int(value);
}

// int16_t
void Reflect(Reader& visitor, int16_t& value) {
  value = (int16_t)visitor.GetInt();
}
void Reflect(Writer& visitor, int16_t& value) {
  visitor.Int(value);
}

// int32_t
void Reflect(Reader& visitor, int32_t& value) {
  value = visitor.GetInt();
}
void Reflect(Writer& visitor, int32_t& value) {
  visitor.Int(value);
}

// int64_t
void Reflect(Reader& visitor, int64_t& value) {
  value = visitor.GetInt64();
}
void Reflect(Writer& visitor, int64_t& value) {
  visitor.Int64(value);
}

// uint64_t
void Reflect(Reader& visitor, uint64_t& value) {
  value = visitor.GetUint64();
}
void Reflect(Writer& visitor, uint64_t& value) {
  visitor.Uint64(value);
}

// double
void Reflect(Reader& visitor, double& value) {
  value = visitor.GetDouble();
}
void Reflect(Writer& visitor, double& value) {
  visitor.Double(value);
}

// bool
void Reflect(Reader& visitor, bool& value) {
  value = visitor.GetBool();
}
void Reflect(Writer& visitor, bool& value) {
  visitor.Bool(value);
}

// std::string
void Reflect(Reader& visitor, std::string& value) {
  if (visitor.IsString())
    value = visitor.GetString();
}
void Reflect(Writer& visitor, std::string& value) {
  visitor.String(value.c_str(), (rapidjson::SizeType)value.size());
}

// ReflectMember
void ReflectMember(Writer& visitor, const char* name, std::string& value) {
  visitor.Key(name);
  Reflect(visitor, value);
}

// TODO: Move this to indexer.cc
void Reflect(Reader& visitor, IndexInclude& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER(line);
  REFLECT_MEMBER(resolved_path);
  REFLECT_MEMBER_END();
}
void Reflect(Writer& visitor, IndexInclude& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER(line);
  if (gTestOutputMode) {
    std::string basename = GetBaseName(value.resolved_path);
    if (!StartsWith(value.resolved_path, "&"))
      basename = "&" + basename;
    REFLECT_MEMBER2("resolved_path", basename);
  } else {
    REFLECT_MEMBER(resolved_path);
  }
  REFLECT_MEMBER_END();
}

template <typename TVisitor>
void Reflect(TVisitor& visitor, IndexType& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER2("id", value.id);
  REFLECT_MEMBER2("usr", value.usr);
  REFLECT_MEMBER2("short_name", value.def.short_name);
  REFLECT_MEMBER2("detailed_name", value.def.detailed_name);
  REFLECT_MEMBER2("kind", value.def.kind);
  REFLECT_MEMBER2("hover", value.def.hover);
  REFLECT_MEMBER2("comments", value.def.comments);
  REFLECT_MEMBER2("definition_spelling", value.def.definition_spelling);
  REFLECT_MEMBER2("definition_extent", value.def.definition_extent);
  REFLECT_MEMBER2("alias_of", value.def.alias_of);
  REFLECT_MEMBER2("parents", value.def.parents);
  REFLECT_MEMBER2("derived", value.derived);
  REFLECT_MEMBER2("types", value.def.types);
  REFLECT_MEMBER2("funcs", value.def.funcs);
  REFLECT_MEMBER2("vars", value.def.vars);
  REFLECT_MEMBER2("instances", value.instances);
  REFLECT_MEMBER2("uses", value.uses);
  REFLECT_MEMBER_END();
}

template <typename TVisitor>
void Reflect(TVisitor& visitor, IndexFunc& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER2("id", value.id);
  REFLECT_MEMBER2("is_operator", value.def.is_operator);
  REFLECT_MEMBER2("usr", value.usr);
  REFLECT_MEMBER2("short_name", value.def.short_name);
  REFLECT_MEMBER2("detailed_name", value.def.detailed_name);
  REFLECT_MEMBER2("kind", value.def.kind);
  REFLECT_MEMBER2("hover", value.def.hover);
  REFLECT_MEMBER2("comments", value.def.comments);
  REFLECT_MEMBER2("declarations", value.declarations);
  REFLECT_MEMBER2("definition_spelling", value.def.definition_spelling);
  REFLECT_MEMBER2("definition_extent", value.def.definition_extent);
  REFLECT_MEMBER2("declaring_type", value.def.declaring_type);
  REFLECT_MEMBER2("base", value.def.base);
  REFLECT_MEMBER2("derived", value.derived);
  REFLECT_MEMBER2("locals", value.def.locals);
  REFLECT_MEMBER2("callers", value.callers);
  REFLECT_MEMBER2("callees", value.def.callees);
  REFLECT_MEMBER_END();
}

template <typename TVisitor>
void Reflect(TVisitor& visitor, IndexVar& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER2("id", value.id);
  REFLECT_MEMBER2("usr", value.usr);
  REFLECT_MEMBER2("short_name", value.def.short_name);
  REFLECT_MEMBER2("detailed_name", value.def.detailed_name);
  REFLECT_MEMBER2("hover", value.def.hover);
  REFLECT_MEMBER2("comments", value.def.comments);
  REFLECT_MEMBER2("declaration", value.def.declaration);
  REFLECT_MEMBER2("definition_spelling", value.def.definition_spelling);
  REFLECT_MEMBER2("definition_extent", value.def.definition_extent);
  REFLECT_MEMBER2("variable_type", value.def.variable_type);
  REFLECT_MEMBER2("declaring_type", value.def.declaring_type);
  REFLECT_MEMBER2("kind", value.def.kind);
  REFLECT_MEMBER2("uses", value.uses);
  REFLECT_MEMBER_END();
}

// IndexFile
bool ReflectMemberStart(Writer& visitor, IndexFile& value) {
  auto it = value.id_cache.usr_to_type_id.find("");
  if (it != value.id_cache.usr_to_type_id.end()) {
    value.Resolve(it->second)->def.short_name = "<fundamental>";
    assert(value.Resolve(it->second)->uses.size() == 0);
  }

  value.version = IndexFile::kCurrentVersion;
  DefaultReflectMemberStart(visitor);
  return true;
}
template <typename TVisitor>
void Reflect(TVisitor& visitor, IndexFile& value) {
  REFLECT_MEMBER_START();
  if (!gTestOutputMode) {
    REFLECT_MEMBER(version);
    REFLECT_MEMBER(last_modification_time);
    REFLECT_MEMBER(language);
    REFLECT_MEMBER(import_file);
    REFLECT_MEMBER(args);
  }
  REFLECT_MEMBER(includes);
  if (!gTestOutputMode)
    REFLECT_MEMBER(dependencies);
  REFLECT_MEMBER(skipped_by_preprocessor);
  REFLECT_MEMBER(types);
  REFLECT_MEMBER(funcs);
  REFLECT_MEMBER(vars);
  REFLECT_MEMBER_END();
}

void Reflect(Reader& visitor, std::monostate&) {
  visitor.GetNull();
}

void Reflect(Writer& visitor, std::monostate&) {
  visitor.Null();
}

void Reflect(Reader& visitor, SerializeFormat& value) {
  std::string fmt = visitor.GetString();
  value = fmt[0] == 'm' ? SerializeFormat::MessagePack : SerializeFormat::Json;
}

void Reflect(Writer& visitor, SerializeFormat& value) {
  switch (value) {
    case SerializeFormat::Json:
      visitor.String("json");
      break;
    case SerializeFormat::MessagePack:
      visitor.String("msgpack");
      break;
  }
}

std::string Serialize(SerializeFormat format, IndexFile& file) {

  switch (format) {
    case SerializeFormat::Json: {
      rapidjson::StringBuffer output;
      rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(output);
      writer.SetFormatOptions(
          rapidjson::PrettyFormatOptions::kFormatSingleLineArray);
      writer.SetIndent(' ', 2);
      JsonWriter json_writer(&writer);
      Reflect(json_writer, file);
      return output.GetString();
    }
    case SerializeFormat::MessagePack: {
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      MessagePackWriter msgpack_writer(&pk);
      Reflect(msgpack_writer, file);
      return std::string(buf.data(), buf.size());
    }
  }
  return "";
}

std::unique_ptr<IndexFile> Deserialize(SerializeFormat format,
                                       std::string path,
                                       std::string serialized,
                                       optional<int> expected_version) {
  std::unique_ptr<IndexFile> file;
  switch (format) {
    case SerializeFormat::Json: {
      rapidjson::Document reader;
      reader.Parse(serialized.c_str());
      if (reader.HasParseError())
        return nullptr;

      // Do not deserialize a document with a bad version. Doing so could cause a
      // crash because the file format may have changed.
      if (expected_version) {
        auto actual_version = reader.FindMember("version");
        if (actual_version == reader.MemberEnd() ||
            actual_version->value.GetInt() != expected_version) {
          return nullptr;
        }
      }

      file = MakeUnique<IndexFile>(path);
      JsonReader json_reader{&reader};
      Reflect(json_reader, *file);
      break;
    }

    case SerializeFormat::MessagePack: {
      if (serialized.empty())
        return nullptr;
      try {
        msgpack::unpacker upk;
        upk.reserve_buffer(serialized.size());
        memcpy(upk.buffer(), serialized.data(), serialized.size());
        upk.buffer_consumed(serialized.size());
        file = MakeUnique<IndexFile>(path);
        MessagePackReader reader(&upk);
        Reflect(reader, *file);
        if (file->version != expected_version)
          return nullptr;
      } catch (msgpack::unpack_error& ex) {
        LOG_S(ERROR) << "msgpack::unpack_err for '" << path << "' " << ex.what();
        return nullptr;
      }
      break;
    }
  }

  // Restore non-serialized state.
  file->path = path;
  file->id_cache.primary_file = file->path;
  for (const auto& type : file->types) {
    file->id_cache.type_id_to_usr[type.id] = type.usr;
    file->id_cache.usr_to_type_id[type.usr] = type.id;
  }
  for (const auto& func : file->funcs) {
    file->id_cache.func_id_to_usr[func.id] = func.usr;
    file->id_cache.usr_to_func_id[func.usr] = func.id;
  }
  for (const auto& var : file->vars) {
    file->id_cache.var_id_to_usr[var.id] = var.usr;
    file->id_cache.usr_to_var_id[var.usr] = var.id;
  }

  return file;
}

void SetTestOutputMode() {
  gTestOutputMode = true;
}

TEST_SUITE("Serializer utils") {
  TEST_CASE("GetBaseName") {
    REQUIRE(GetBaseName("foo.cc") == "foo.cc");
    REQUIRE(GetBaseName("foo/foo.cc") == "foo.cc");
    REQUIRE(GetBaseName("/foo.cc") == "foo.cc");
    REQUIRE(GetBaseName("///foo.cc") == "foo.cc");
    REQUIRE(GetBaseName("bar/") == "bar/");
    REQUIRE(GetBaseName("foobar/bar/") ==
            "foobar/bar/");  // TODO: Should be bar, but good enough.
  }
}
