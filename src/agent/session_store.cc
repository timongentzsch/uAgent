// Copyright 2026 Timon Gentzsch

#include "include/agent/session_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/adaptive_system.h"
#include "include/agent/conversation.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/lease.h"
#include "include/core/strings.h"
#include "include/tools/files.h"

namespace uagent {
namespace {

SessionStoreStatus Error(SessionStoreError code, std::string message) {
  return {code, std::move(message)};
}

struct Field {
  const char* key;
  json::value_t type;
  bool required;
};

// number_integer also admits number_unsigned, mirroring is_number_integer().
bool HasFields(const json& value, std::span<const Field> fields) {
  if (!value.is_object()) return false;
  for (const Field& field : fields) {
    const auto entry = value.find(field.key);
    if (entry == value.end()) {
      if (field.required) return false;
    } else if (entry->type() != field.type &&
               !(field.type == json::value_t::number_integer &&
                 entry->is_number_integer())) {
      return false;
    }
  }
  return true;
}

constexpr Field kStateFields[] = {
    {"messages", json::value_t::array, true},
    {"message_kinds", json::value_t::array, true},
    {"archive", json::value_t::array, true},
    {"archive_dropped_segments", json::value_t::number_integer, true},
    {"context_tokens", json::value_t::number_integer, true},
    {"usage", json::value_t::object, true},
    {"adaptive_system", json::value_t::string, false},
    {"adaptive_system_revision", json::value_t::number_unsigned, false},
    {"tool_displays", json::value_t::object, true},
    {"display", json::value_t::object, false}};

constexpr Field kHeaderFields[] = {
    {kSessionHeaderCwd, json::value_t::string, true},
    {kSessionHeaderModel, json::value_t::string, true},
    {kSessionHeaderSessionId, json::value_t::string, true},
    {kSessionHeaderTurns, json::value_t::number_integer, true},
    {kSessionHeaderTitle, json::value_t::string, true}};

bool ValidState(const json& value) {
  if (!HasFields(value, kStateFields)) return false;
  const auto adaptive = value.find("adaptive_system");
  if (adaptive != value.end() &&
      adaptive->get_ref<const std::string&>().size() > kAdaptiveSystemBytes) {
    return false;
  }
  return !value["messages"].empty() &&
         value["message_kinds"].size() == value["messages"].size();
}

bool ValidHeader(const json& header) {
  return HasFields(header, kHeaderFields);
}

json HeaderJson(const SessionMetadata& metadata) {
  return {{"format", kSessionFormat},
          {kSessionHeaderCwd, metadata.cwd},
          {kSessionHeaderModel, metadata.model},
          {kSessionHeaderSessionId, metadata.session_id},
          {kSessionHeaderTurns, metadata.turns},
          {kSessionHeaderTitle, metadata.title},
          {"custom_title", metadata.custom_title}};
}

json StateJson(const SessionState& state) {
  return {{"messages", state.messages},
          {"message_kinds", MessageKindsJson(state.message_kinds)},
          {"archive", state.archive},
          {"archive_dropped_segments", state.archive_dropped_segments},
          {"context_tokens", state.context_tokens},
          {"usage", UsageJson(state.usage)},
          {"route_usage", RouteUsageJson(state.route_usage)},
          {"adaptive_system", state.adaptive_system},
          {"adaptive_system_revision", state.adaptive_system_revision},
          {"tool_displays", state.tool_displays},
          {"display", state.display}};
}

}  // namespace

SessionStoreStatus SessionStore::Save(const std::string& path,
                                      const SessionRecord& record) {
  json header = HeaderJson(record.metadata);
  header["incoming"] =
      JsonValue(JsonValue(record.state.display, "statistics", json::object()),
                "incoming", uint64_t{0});
  json state = StateJson(record.state);
  // The header is built from a typed struct; only the state can be incomplete.
  if (!ValidState(state)) {
    return Error(SessionStoreError::kInvalid,
                 "refusing to save incomplete session state");
  }
  std::string header_text = JsonDump(header);
  std::string body = JsonDump(state);
  if (header_text.size() >= kSessionHeaderBytes ||
      body.size() > kSessionReadBytes -
                        std::min(kSessionReadBytes, header_text.size() + 1)) {
    return Error(SessionStoreError::kInvalid,
                 "session exceeds readable storage limits");
  }
  ToolResult result = ToolWritePrivateFile(path, header_text + "\n" + body);
  if (!result.Ok()) return Error(SessionStoreError::kIo, result.output);
  return {};
}

SessionLoadResult SessionStore::Load(const std::string& path,
                                     const std::string& expected_cwd) {
  SessionLoadResult result = Inspect(path);
  if (!result.record) return result;
  const auto saved = CanonicalAccessPath(result.record->metadata.cwd);
  const auto expected = CanonicalAccessPath(expected_cwd);
  if (saved != expected) {
    return {Error(SessionStoreError::kWrongWorkspace,
                  "session belongs to " + saved.string() + ", not " +
                      expected.string()),
            std::nullopt};
  }
  return result;
}

SessionLoadResult SessionStore::Inspect(const std::string& path) {
  std::string content, error;
  if (!ReadRegularFile(path, kSessionReadBytes, content, error)) {
    return {Error(PathExists(path) ? SessionStoreError::kIo
                                   : SessionStoreError::kNotFound,
                  error),
            std::nullopt};
  }
  size_t newline = content.find('\n');
  if (newline > kSessionHeaderBytes) {
    return {Error(SessionStoreError::kCorrupt,
                  "session header is incomplete or too large"),
            std::nullopt};
  }
  std::string_view header_line(content.data(), newline);
  std::string_view body(content.data() + newline + 1,
                        content.size() - newline - 1);
  if (body.empty()) {
    return {Error(SessionStoreError::kCorrupt, "session is incomplete"),
            std::nullopt};
  }

  json header = json::parse(header_line, nullptr, false);
  if (!ValidHeader(header) || !header.contains("format") ||
      !header["format"].is_number_integer()) {
    return {Error(SessionStoreError::kCorrupt, "session header is invalid"),
            std::nullopt};
  }
  int64_t format = header["format"].get<int64_t>();
  if (format != kSessionFormat) {
    return {Error(SessionStoreError::kIncompatible,
                  "unsupported session format " + std::to_string(format)),
            std::nullopt};
  }

  json state = json::parse(body, nullptr, false);
  if (state.is_discarded() || !ValidState(state)) {
    return {Error(SessionStoreError::kCorrupt,
                  "session payload is invalid or incomplete"),
            std::nullopt};
  }
  std::vector<MessageKind> message_kinds;
  if (!ParseMessageKinds(state["message_kinds"], state["messages"].size(),
                         message_kinds)) {
    return {Error(SessionStoreError::kCorrupt,
                  "session message metadata is invalid or incomplete"),
            std::nullopt};
  }

  SessionRecord record;
  record.metadata.cwd = header[kSessionHeaderCwd].get<std::string>();
  record.metadata.model = header[kSessionHeaderModel].get<std::string>();
  record.metadata.session_id =
      header[kSessionHeaderSessionId].get<std::string>();
  record.metadata.turns = header[kSessionHeaderTurns].get<int64_t>();
  record.metadata.title = header[kSessionHeaderTitle].get<std::string>();
  record.metadata.custom_title = JsonValue(header, "custom_title", false);
  record.state.messages = std::move(state["messages"]);
  record.state.message_kinds = std::move(message_kinds);
  record.state.archive = std::move(state["archive"]);
  record.state.archive_dropped_segments =
      std::max(int64_t{0}, state["archive_dropped_segments"].get<int64_t>());
  record.state.context_tokens =
      std::max(int64_t{0}, state["context_tokens"].get<int64_t>());
  record.state.usage = UsageFromJson(state["usage"]);
  record.state.route_usage =
      RouteUsageFromJson(JsonValue(state, "route_usage", json::object()));
  record.state.adaptive_system = JsonValue(state, "adaptive_system", "");
  record.state.adaptive_system_revision =
      JsonValue(state, "adaptive_system_revision", uint64_t{0});
  record.state.tool_displays = std::move(state["tool_displays"]);
  record.state.display = JsonValue(state, "display", json::object());
  return {{}, std::move(record)};
}

std::vector<SessionInfo> ListSessions(SessionScope scope) {
  namespace fs = std::filesystem;
  constexpr size_t kCatalogueLimit = 4096;
  const std::string current = CanonicalCwd();
  const fs::path base = fs::path(GlobalBase()) / kHistoryDir;
  std::vector<fs::path> directories{base};
  std::error_code ec;
  if (scope == SessionScope::kWorkspace) {
    directories.push_back(base / WorkspaceId(current));
  } else {
    for (const auto& entry : fs::directory_iterator(base, ec)) {
      if (directories.size() >= kCatalogueLimit) break;
      if (fs::is_directory(entry.symlink_status(ec))) {
        directories.push_back(entry.path());
      }
    }
  }
  std::vector<SessionInfo> out;
  for (const fs::path& directory : directories) {
    if (!fs::is_directory(fs::symlink_status(directory, ec))) continue;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
      if (out.size() >= kCatalogueLimit) break;
      if (entry.path().extension() != ".json" ||
          !fs::is_regular_file(entry.symlink_status(ec))) {
        continue;
      }
      SessionInfo item;
      item.path = entry.path().string();
      item.mtime = entry.last_write_time(ec);
      auto bytes = entry.file_size(ec);
      item.bytes =
          ec ? 0 : static_cast<int64_t>(std::min(bytes, uintmax_t{INT64_MAX}));
      std::string prefix, error;
      json header;
      if (ReadRegularFile(item.path, kSessionHeaderBytes, prefix, error,
                          true)) {
        size_t newline = prefix.find('\n');
        if (newline != std::string::npos) {
          header = json::parse(prefix.substr(0, newline), nullptr, false);
        }
      }
      if (!ValidHeader(header)) {
        item.title = entry.path().filename().string();
        item.error = "invalid or oversized session header";
      } else {
        item.cwd = JsonValue(header, kSessionHeaderCwd, "");
        item.title = JsonValue(header, kSessionHeaderTitle, "(untitled)");
        item.turns = JsonValue(header, kSessionHeaderTurns, int64_t{0});
        item.incoming = JsonValue(header, "incoming", uint64_t{0});
        if (JsonValue(header, "format", int64_t{0}) != kSessionFormat) {
          item.error = "unsupported session format";
        }
      }
      if (scope == SessionScope::kAll || item.cwd == current) {
        out.push_back(std::move(item));
      }
    }
  }
  std::sort(out.begin(), out.end(),
            [](const SessionInfo& a, const SessionInfo& b) {
              return a.mtime == b.mtime ? a.path < b.path : a.mtime > b.mtime;
            });
  return out;
}

bool ValidSessionTitle(const std::string& title) {
  return !title.empty() && title.size() <= 256 &&
         std::none_of(title.begin(), title.end(),
                      [](unsigned char ch) { return ch < 32 || ch == 127; });
}

json SessionStore::Fork(const std::string& source, const std::string& title,
                        bool source_owned) {
  FileLease writer;
  std::string error;
  if (!source_owned &&
      !writer.Acquire(CanonicalAccessPath(source).string() + ".lock", error)) {
    return {{"error", error}};
  }
  auto loaded = Inspect(source);
  if (!loaded.record) return {{"error", loaded.status.message}};
  if (!title.empty() && !ValidSessionTitle(title)) {
    return {{"error", "invalid fork name"}};
  }
  auto record = std::move(*loaded.record);
  Conversation conversation;
  if (!conversation.Restore(record.state.messages, record.state.message_kinds,
                            record.state.archive,
                            record.state.archive_dropped_segments,
                            record.state.tool_displays, record.state.display)) {
    return {{"error", "session conversation state is invalid"}};
  }
  // Older format-3 sessions have no display metadata. Normalize it before
  // recording fork facts so the fork can be restored like any conversation.
  record.state.display = conversation.DisplayMetadata();
  const std::string identity = MakeSessionId();
  const std::string path = (std::filesystem::path(source).parent_path() /
                            ("fork-" + identity + ".json"))
                               .string();
  std::error_code ec;
  std::map<std::string, std::string> copies;
  // Owned assets are immutable; independent directory entries make deletion
  // independent without a reference-count database. Metadata is copied.
  if (std::filesystem::is_symlink(
          std::filesystem::symlink_status(source + ".assets", ec))) {
    return {{"error", "unsafe source attachment directory"}};
  }
  ec.clear();
  if (std::filesystem::exists(source + ".assets", ec)) {
    CreatePrivateDirectories(path + ".assets");
    for (const auto& entry :
         std::filesystem::directory_iterator(source + ".assets", ec)) {
      if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) {
        ec = std::make_error_code(std::errc::invalid_argument);
        break;
      }
      auto destination =
          std::filesystem::path(path + ".assets") / entry.path().filename();
      std::filesystem::copy_file(entry.path(), destination, ec);
      if (ec) break;
    }
  }
  auto artifacts = CanonicalAccessPath(UagentDir(kArtifactsDir));
  std::function<void(json&)> rewrite = [&](json& value) {
    if (ec) return;
    if (value.is_object() || value.is_array()) {
      for (json& child : value) rewrite(child);
    } else if (value.is_string()) {
      std::string text = value.get<std::string>();
      const auto file = text.size() < 4096 && text.starts_with("/")
                            ? std::filesystem::path(text)
                            : std::filesystem::path();
      if ((file.filename().string().starts_with("http-") ||
           file.filename().string().starts_with("exchange-")) &&
          CanonicalAccessPath(file.parent_path()) == artifacts) {
        auto [it, added] = copies.try_emplace(text);
        if (added) {
          Fd copy(CreateTempFile(
              (artifacts / (file.filename().string().starts_with("http-")
                                ? "http-XXXXXX"
                                : "exchange-XXXXXX"))
                  .string(),
              it->second));
          if (!copy) {
            ec = std::make_error_code(std::errc::io_error);
            return;
          }
          Fd input(open(text.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
          struct stat info{};
          if (!input || fstat(input.Get(), &info) != 0 ||
              !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
              info.st_size < 0 ||
              static_cast<uint64_t>(info.st_size) > kSessionReadBytes) {
            unlink(it->second.c_str());
            it->second.clear();
          } else {
            char buffer[16384];
            size_t copied = 0;
            for (;;) {
              ssize_t count = read(input.Get(), buffer, sizeof(buffer));
              if (count < 0 && errno == EINTR) continue;
              if (count == 0) break;
              if (count < 0 ||
                  static_cast<size_t>(count) > kSessionReadBytes - copied ||
                  !WriteFully(
                      copy.Get(),
                      std::string_view(buffer, static_cast<size_t>(count)))) {
                ec = std::make_error_code(std::errc::io_error);
                break;
              }
              copied += static_cast<size_t>(count);
            }
          }
        }
        value = it->second;
      } else {
        const std::string from = source + ".assets/", to = path + ".assets/";
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
          text.replace(pos, from.size(), to);
          pos += to.size();
        }
        value = std::move(text);
      }
    }
  };
  rewrite(record.state.messages);
  rewrite(record.state.archive);
  rewrite(record.state.display);
  record.state.display["facts"]["fork-origin"] = {
      {"source", HashHex(source)},
      {"title", record.metadata.title},
      {"turns", record.metadata.turns},
      {"usage", UsageJson(record.state.usage)},
      {"statistics", record.state.display["statistics"]}};
  record.state.usage = {};
  record.state.route_usage.clear();
  record.state.display["statistics"] = {{"incoming", 0}, {"complete", true}};
  record.metadata.session_id = identity;
  record.metadata.title =
      title.empty() ? Utf8Prefix("Fork of " + record.metadata.title, 256)
                    : title;
  record.metadata.custom_title = true;
  auto result =
      ec ? Error(SessionStoreError::kIo, ec.message()) : Save(path, record);
  if (!result.Ok()) {
    std::filesystem::remove_all(path + ".assets", ec);
    for (const auto& [original, copy] : copies) {
      if (!copy.empty()) unlink(copy.c_str());
    }
    return {{"error", result.message}};
  }
  return {{"forked", true},
          {"id", HashHex(path)},
          {"path", path},
          {"cwd", record.metadata.cwd},
          {"title", record.metadata.title}};
}

SessionStoreStatus SessionStore::Rename(const std::string& path,
                                        const std::string& title) {
  if (!ValidSessionTitle(title)) {
    return Error(SessionStoreError::kInvalid,
                 "title must be 1–256 bytes without control characters");
  }
  FileLease writer;
  std::string error;
  if (!writer.Acquire(CanonicalAccessPath(path).string() + ".lock", error)) {
    return Error(SessionStoreError::kIo, std::move(error));
  }
  auto loaded = Inspect(path);
  if (!loaded.status.Ok() || !loaded.record) return loaded.status;
  loaded.record->metadata.title = title;
  loaded.record->metadata.custom_title = true;
  return Save(path, *loaded.record);
}

SessionStoreStatus SessionStore::Remove(const std::string& path,
                                        const std::string& draft_path) {
  FileLease writer;
  std::string error;
  if (!writer.Acquire(CanonicalAccessPath(path).string() + ".lock", error)) {
    return Error(SessionStoreError::kIo, std::move(error));
  }
  auto loaded = Inspect(path);
  std::error_code ec;
  // Remove the optional web draft before the snapshot so it cannot resurrect
  // a completed deletion. Acquire ownership before changing any files.
  if (!draft_path.empty()) std::filesystem::remove(draft_path, ec);
  // These are session-owned siblings; remove_all never follows symlinks.
  if (!ec) std::filesystem::remove_all(path + ".assets", ec);
  if (!ec) std::filesystem::remove(path + ".events.jsonl", ec);
  if (!ec) std::filesystem::remove(path, ec);
  if (!ec && loaded.record) {
    const json facts =
        JsonValue(loaded.record->state.display, "facts", json::object());
    const auto artifacts = CanonicalAccessPath(UagentDir(kArtifactsDir));
    std::function<void(const json&)> remove_artifacts = [&](const json& value) {
      if (value.is_object() || value.is_array()) {
        for (const json& child : value) remove_artifacts(child);
      } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() >= 4096 || !text.starts_with("/")) return;
        const std::filesystem::path body(text);
        if ((body.filename().string().starts_with("exchange-") ||
             body.filename().string().starts_with("http-")) &&
            CanonicalAccessPath(body.parent_path()) == artifacts) {
          std::error_code ignored;
          std::filesystem::remove(artifacts / body.filename(), ignored);
        }
      }
    };
    remove_artifacts(facts);
  }
  return ec ? Error(SessionStoreError::kIo, ec.message())
            : SessionStoreStatus{};
}

}  // namespace uagent
