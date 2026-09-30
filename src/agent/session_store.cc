// Copyright 2026 Timon Gentzsch

#include "include/agent/session_store.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/file_services.h"
#include "include/agent/session_role.h"
#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/lease.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
namespace {
// A retained HTTP body in the private artifacts directory, as display facts
// name it; anything else a fact holds is left alone.
bool ArtifactPath(const std::string& text,
                  const std::filesystem::path& artifacts) {
  if (text.size() >= kRetainedArtifactPathChars || !text.starts_with("/")) {
    return false;
  }
  const std::filesystem::path file(text);
  const std::string name = file.filename().string();
  return (name.starts_with("http-") || name.starts_with("exchange-")) &&
         CanonicalAccessPath(file.parent_path()) == artifacts;
}

// Visits every string leaf of a JSON value, depth first.
template <typename Json, typename Visit>
void ForEachString(Json& value, const Visit& visit) {
  if (value.is_object() || value.is_array()) {
    for (auto& child : value) ForEachString(child, visit);
  } else if (value.is_string()) {
    visit(value);
  }
}


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
    {"context_window", json::value_t::number_integer, false},
    {"usage", json::value_t::object, true},
    {"last_sent_prompt", json::value_t::string, false},
    {"adaptive_system", json::value_t::string, false},
    {"adaptive_system_revision", json::value_t::number_unsigned, false},
    {"adaptive_system_mode", json::value_t::string, false},
    {"tool_displays", json::value_t::object, false},
    {"display", json::value_t::object, false},
    {"delivered_mail", json::value_t::array, false}};

constexpr Field kHeaderFields[] = {
    {kSessionHeaderCwd, json::value_t::string, true},
    {kSessionHeaderModel, json::value_t::string, true},
    {kSessionHeaderSessionId, json::value_t::string, true},
    {kSessionHeaderTurns, json::value_t::number_integer, true},
    {kSessionHeaderTitle, json::value_t::string, true}};

bool ValidState(const SessionState& state,
                const Conversation* conversation = nullptr) {
  const json& messages =
      conversation ? conversation->Messages() : state.messages;
  return messages.is_array() && !messages.empty() &&
         (conversation ? conversation->Kinds() : state.message_kinds).size() ==
             messages.size() &&
         (conversation ? conversation->Archive() : state.archive).is_array() &&
         (conversation ? conversation->ToolDisplays() : state.tool_displays)
             .is_object() &&
         state.display.is_object() && state.delivered_mail.is_array() &&
         state.adaptive_system.size() <= kAdaptiveSystemBytes &&
         (state.adaptive_system_mode == "overlay" ||
          state.adaptive_system_mode == "replace");
}

bool ValidHeader(const json& header) {
  return HasFields(header, kHeaderFields);
}

json HeaderJson(const SessionMetadata& metadata) {
  json header = {{"format", kSessionFormat},
                 {kSessionHeaderCwd, metadata.cwd},
                 {kSessionHeaderModel, metadata.model},
                 {kSessionHeaderSessionId, metadata.session_id},
                 {kSessionHeaderTurns, metadata.turns},
                 {kSessionHeaderTitle, metadata.title},
                 {"custom_title", metadata.custom_title},
                 {kSessionHeaderParent, metadata.parent_session_id},
                 {kSessionHeaderForkTurn, metadata.forked_at_turn},
                 {kSessionHeaderForkTime, metadata.forked_at_time}};
  if (!metadata.delegation.empty()) {
    header[kSessionHeaderDelegation] = metadata.delegation;
  }
  if (!metadata.kind.empty()) header[kSessionHeaderKind] = metadata.kind;
  if (!metadata.thread.empty()) header[kSessionHeaderThread] = metadata.thread;
  return header;
}

std::string StateText(const SessionState& state,
                      const Conversation* conversation) {
  json value = {
      {"message_kinds", MessageKindsJson(conversation ? conversation->Kinds()
                                                      : state.message_kinds)},
      {"archive_dropped_segments", conversation
                                       ? conversation->DroppedSegments()
                                       : state.archive_dropped_segments},
      {"context_tokens", state.context_tokens},
      {"context_window", state.context_window},
      {"usage", UsageJson(state.usage)},
      {"route_usage", RouteUsageJson(state.route_usage)},
      {"last_sent_prompt", state.last_sent_prompt},
      {"adaptive_system", state.adaptive_system},
      {"adaptive_system_mode", state.adaptive_system_mode},
      {"adaptive_system_revision", state.adaptive_system_revision},
      {"delivered_mail", state.delivered_mail}};
  // Serialize borrowed arrays directly: the live transcript stays in place.
  std::string text = JsonDump(value);
  text.pop_back();
  for (const auto& [name, field] :
       {std::pair{"display", &state.display},
        {"messages",
         conversation ? &conversation->Messages() : &state.messages},
        {"tool_displays", conversation ? &conversation->ToolDisplays()
                                       : &state.tool_displays}}) {
    text += "," + JsonDump(name) + ":" + JsonDump(*field);
  }
  // The live archive keeps its segments serialized since archiving.
  return text + ",\"archive\":" +
         (conversation ? conversation->ArchiveText() : JsonDump(state.archive)) +
         "}";
}

}  // namespace

bool SessionState::RestoreConversation(Conversation& conversation) && {
  return conversation.Restore(std::move(messages), std::move(message_kinds),
                              std::move(archive), archive_dropped_segments,
                              std::move(tool_displays), display);
}

SessionStoreStatus SessionStore::Save(const std::string& path,
                                      const SessionRecord& record,
                                      const Conversation* conversation) {
  json header = HeaderJson(record.metadata);
  header["incoming"] =
      JsonValue(JsonValue(record.state.display, "statistics", json::object()),
                "incoming", uint64_t{0});
  // Spend is read across many sessions (a coordinator's daily limit), so it
  // rides in the header instead of needing the whole state parsed.
  header["cost"] = record.state.usage.cost;
  // The header is built from a typed struct; only the state can be incomplete.
  if (!ValidState(record.state, conversation)) {
    return Error(SessionStoreError::kInvalid,
                 "refusing to save incomplete session state");
  }
  std::string header_text = JsonDump(header);
  std::string body = StateText(record.state, conversation);
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
  if (state.is_discarded() || !HasFields(state, kStateFields)) {
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
  record.metadata.parent_session_id =
      JsonValue(header, kSessionHeaderParent, "");
  record.metadata.forked_at_turn =
      JsonValue(header, kSessionHeaderForkTurn, int64_t{0});
  record.metadata.forked_at_time =
      JsonValue(header, kSessionHeaderForkTime, "");
  record.metadata.delegation =
      JsonValue(header, kSessionHeaderDelegation, json::object());
  record.metadata.kind = JsonValue(header, kSessionHeaderKind, "");
  record.metadata.thread =
      JsonValue(header, kSessionHeaderThread, json::object());
  record.state.messages = std::move(state["messages"]);
  record.state.message_kinds = std::move(message_kinds);
  record.state.archive = std::move(state["archive"]);
  record.state.archive_dropped_segments =
      std::max(int64_t{0}, state["archive_dropped_segments"].get<int64_t>());
  record.state.context_tokens =
      std::max(int64_t{0}, state["context_tokens"].get<int64_t>());
  record.state.context_window =
      std::max(int64_t{0}, JsonValue(state, "context_window", int64_t{0}));
  record.state.usage = UsageFromJson(state["usage"]);
  record.state.route_usage =
      RouteUsageFromJson(JsonValue(state, "route_usage", json::object()));
  record.state.last_sent_prompt = JsonValue(state, "last_sent_prompt", "");
  record.state.adaptive_system = JsonValue(state, "adaptive_system", "");
  record.state.adaptive_system_mode =
      JsonValue(state, "adaptive_system_mode", "overlay");
  record.state.adaptive_system_revision =
      JsonValue(state, "adaptive_system_revision", uint64_t{0});
  if (state.contains("tool_displays")) {
    record.state.tool_displays = std::move(state["tool_displays"]);
  }
  if (state.contains("display")) {
    record.state.display = std::move(state["display"]);
  }
  record.state.delivered_mail =
      JsonValue(state, "delivered_mail", json::array());
  if (!ValidState(record.state)) {
    return {Error(SessionStoreError::kCorrupt,
                  "session payload is invalid or incomplete"),
            std::nullopt};
  }
  return {{}, std::move(record)};
}

json SessionHeader(const std::string& path) {
  std::string prefix, error;
  if (!ReadRegularFile(path, kSessionHeaderBytes, prefix, error, true)) {
    return json::object();
  }
  const size_t newline = prefix.find('\n');
  if (newline == std::string::npos) return json::object();
  json header = json::parse(prefix.substr(0, newline), nullptr, false);
  return ValidHeader(header) ? header : json::object();
}

std::string CoordinatorPath(const std::string& cwd) {
  return (std::filesystem::path(GlobalBase()) / kHistoryDir / WorkspaceId(cwd) /
          "coordinator.json")
      .string();
}

std::string SessionLockPath(const std::string& path) {
  return CanonicalAccessPath(path).string() + ".lock";
}

std::vector<SessionInfo> ListSessions(SessionScope scope) {
  // One cache per process: a repeated listing (a coordinator's board every
  // step) re-reads only the headers that changed.
  static std::mutex mutex;
  static SessionCatalogue catalogue;
  std::lock_guard lock(mutex);
  return catalogue.List(scope);
}

std::vector<SessionInfo> SessionCatalogue::List(SessionScope scope) {
  namespace fs = std::filesystem;
  constexpr size_t kCatalogueLimit = 4096;
  const std::string current = CanonicalCwd();
  const fs::path base = fs::path(GlobalBase()) / kHistoryDir;
  std::vector<fs::path> directories{base};
  std::error_code ec;
  if (scope != SessionScope::kAll) {
    directories.push_back(base / WorkspaceId(current));
  } else {
    for (const auto& entry : fs::directory_iterator(base, ec)) {
      if (directories.size() >= kCatalogueLimit) break;
      if (fs::is_directory(entry.symlink_status(ec))) {
        directories.push_back(entry.path());
      }
    }
  }
  // Every scope but kAll, the host's whole view, leaves coordinators out.
  const auto listed = [&](const SessionInfo& info) {
    return (scope == SessionScope::kAll ||
            (info.cwd == current && info.kind != kSessionKindCoordinator)) &&
           info.delegation.empty() != (scope == SessionScope::kChildren);
  };
  std::vector<SessionInfo> out;
  std::map<std::string, Entry> next;
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
      const FileStamp stamp = SnapshotFile(item.path);
      auto cached = entries_.find(item.path);
      if (cached != entries_.end() && cached->second.stamp == stamp) {
        if (listed(cached->second.info)) {
          out.push_back(cached->second.info);
          next.insert(entries_.extract(cached));
        }
        continue;
      }
      item.mtime = entry.last_write_time(ec);
      auto bytes = entry.file_size(ec);
      item.bytes =
          ec ? 0 : static_cast<int64_t>(std::min(bytes, uintmax_t{INT64_MAX}));
      const json header = SessionHeader(item.path);
      if (header.empty()) {
        item.title = entry.path().filename().string();
        item.error = "invalid or oversized session header";
      } else {
        item.cwd = JsonValue(header, kSessionHeaderCwd, "");
        item.title = JsonValue(header, kSessionHeaderTitle, "(untitled)");
        item.turns = JsonValue(header, kSessionHeaderTurns, int64_t{0});
        item.incoming = JsonValue(header, "incoming", uint64_t{0});
        item.cost = JsonValue(header, "cost", 0.0);
        item.delegation =
            JsonValue(header, kSessionHeaderDelegation, json::object());
        item.kind = JsonValue(header, kSessionHeaderKind, "");
        item.thread = JsonValue(header, kSessionHeaderThread, json::object());
        if (JsonValue(header, "format", int64_t{0}) != kSessionFormat) {
          item.error = "unsupported session format";
        }
      }
      if (listed(item)) {
        if (stamp.size >= 0 && item.error.empty() &&
            SnapshotFile(item.path) == stamp) {
          next.emplace(item.path, Entry{stamp, item});
        }
        out.push_back(std::move(item));
      }
    }
  }
  entries_ = std::move(next);
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
                        bool source_owned, int64_t fork_turn,
                        const std::string& message_id) {
  FileLease writer;
  std::string error;
  if (!source_owned &&
      !writer.Acquire(SessionLockPath(source), error)) {
    return {{"error", error}};
  }
  auto loaded = Inspect(source);
  if (!loaded.record) return {{"error", loaded.status.message}};
  if (!title.empty() && !ValidSessionTitle(title)) {
    return {{"error", "invalid fork name"}};
  }
  if (fork_turn < 0) {
    return {{"error", "fork turn must be positive"}};
  }
  auto record = std::move(*loaded.record);
  // Captured before the move below overwrites identity and turns.
  const std::string parent_session_id = record.metadata.session_id;
  const int64_t parent_turns = record.metadata.turns;
  Conversation conversation;
  if (!conversation.Restore(record.state.messages, record.state.message_kinds,
                            record.state.archive,
                            record.state.archive_dropped_segments,
                            record.state.tool_displays, record.state.display)) {
    return {{"error", "session conversation state is invalid"}};
  }
  if (!message_id.empty()) {
    uint64_t id = 0;
    const auto [end, parsed] = std::from_chars(
        message_id.data() + std::min<size_t>(2, message_id.size()),
        message_id.data() + message_id.size(), id);
    fork_turn = message_id.starts_with("m-") && parsed == std::errc() &&
                        end == message_id.data() + message_id.size()
                    ? conversation.UserMessageNumber(id)
                    : 0;
    if (fork_turn == 0) {
      return {{"error", "that message is no longer in the live conversation"}};
    }
  }
  // Keeps the prefix before the Nth user message (exclusive, so it can be
  // edited and sent again); 0 forks the whole session.
  const std::string prompt = conversation.UserMessageText(fork_turn);
  if (fork_turn > 0) {
    if (!conversation.TruncateBeforeUserTurn(fork_turn)) {
      return {{"error", "session has fewer than " + std::to_string(fork_turn) +
                            " turns"}};
    }
    // Orphaned tool displays are tolerated like any other Erase caller:
    // they are keyed lookups, bounded, and never render without a message.
    record.state.messages = conversation.Messages();
    record.state.message_kinds = conversation.Kinds();
    record.state.tool_displays = conversation.ToolDisplays();
    record.metadata.turns = fork_turn - 1;
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
  const auto rewrite = [&](json& value) {
    if (ec) return;
    std::string text = value.get<std::string>();
    const std::filesystem::path file(text);
    if (ArtifactPath(text, artifacts)) {
      auto [it, added] = copies.try_emplace(text);
      if (added) {
        ScopedTempFile copy(
            (artifacts / (file.filename().string().starts_with("http-")
                              ? "http-XXXXXX"
                              : "exchange-XXXXXX"))
                .string());
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
          // Kept even when partial: the previous hand-rolled cleanup
          // removed only rejected inputs, not short copies.
          it->second = copy.Release();
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
  };
  ForEachString(record.state.messages, rewrite);
  ForEachString(record.state.archive, rewrite);
  ForEachString(record.state.display, rewrite);
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
  record.metadata.parent_session_id = parent_session_id;
  // A fork is the user's own conversation: never a second coordinator, and
  // no longer a thread its coordinator steers.
  record.metadata.kind.clear();
  record.metadata.thread = json::object();
  record.metadata.forked_at_turn = fork_turn > 0 ? fork_turn : parent_turns;
  record.metadata.forked_at_time = UtcStamp("%Y%m%dT%H%M%SZ");
  if (!title.empty()) {
    record.metadata.title = title;
  } else if (fork_turn > 0) {
    record.metadata.title =
        Utf8Prefix("Fork of " + record.metadata.title + " @ turn " +
                       std::to_string(fork_turn),
                   256);
  } else {
    record.metadata.title = Utf8Prefix("Fork of " + record.metadata.title, 256);
  }
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
  json forked = {{"forked", true},
                 {"id", HashHex(path)},
                 {"path", path},
                 {"cwd", record.metadata.cwd},
                 {"title", record.metadata.title}};
  if (fork_turn > 0) forked["prompt"] = prompt;
  return forked;
}

namespace {
// Transcript text for /share: plain strings pass through, content arrays
// contribute their text parts and file placeholders for the rest.
std::string ShareText(const json& message) {
  if (!message.is_object()) return "";
  auto content = message.find("content");
  if (content == message.end()) return "";
  if (content->is_string()) return content->get<std::string>();
  if (!content->is_array()) return "";
  std::string out;
  for (const json& part : *content) {
    if (!part.is_object()) continue;
    const std::string type = JsonValue(part, "type", "");
    if (type == "text") {
      const std::string text = JsonValue(part, "text", "");
      if (!text.empty()) {
        if (!out.empty()) out += "\n";
        out += text;
      }
    } else if (type == "attachment" || type == "image" || type == "file") {
      const std::string ref =
          JsonValue(part, "path", JsonValue(part, "name", ""));
      if (!out.empty()) out += "\n";
      out += "[file: " + (ref.empty() ? type : ref) + "]";
    }
  }
  return out;
}
}  // namespace

std::string SessionStore::ShareMarkdown(const SessionRecord& record) {
  const std::string title = Trim(record.metadata.title);
  std::string out =
      "# " + (title.empty() ? "(untitled session)" : title) + "\n\n";
  out += "_" + std::to_string(std::max<int64_t>(0, record.metadata.turns)) +
         " user turns";
  if (!record.metadata.model.empty()) out += " · " + record.metadata.model;
  if (!record.metadata.parent_session_id.empty()) {
    out += " · forked from " + record.metadata.parent_session_id + " at turn " +
           std::to_string(record.metadata.forked_at_turn);
  }
  out += "_\n";
  int64_t user_n = 0;
  for (size_t index = 0; index < record.state.messages.size() &&
                         index < record.state.message_kinds.size();
       ++index) {
    const MessageKind kind = record.state.message_kinds[index];
    const json& message = record.state.messages[index];
    if (IsUserMessage(message, kind)) {
      out += "\n## User " + std::to_string(++user_n) + "\n\n" +
             ShareText(message) + "\n";
    } else if (kind == MessageKind::kAttachment) {
      out += "\n## Attached\n\n" + ShareText(message) + "\n";
    } else if (kind == MessageKind::kAssistant) {
      const std::string text = ShareText(message);
      if (text.empty()) continue;  // Tool-call-only message.
      out += "\n## Assistant\n\n" + text + "\n";
    } else if (kind == MessageKind::kToolResult) {
      std::string text = ShareText(message);
      if (text.empty()) continue;
      if (text.size() > kSharedToolResultChars) {
        text = Utf8Prefix(text, kSharedToolResultChars) + "\n...[truncated]";
      }
      const std::string name = JsonValue(message, "name", "");
      out += "\n### tool" + (name.empty() ? "" : " `" + name + "`") +
             "\n\n```\n" + text + "\n```\n";
    }
  }
  return out;
}

json SessionStore::Share(const std::string& path) {
  auto loaded = Inspect(path);
  if (!loaded.record) return {{"error", loaded.status.message}};
  std::string sibling = path;
  const std::string suffix = ".json";
  if (sibling.size() > suffix.size() && sibling.ends_with(suffix)) {
    sibling.resize(sibling.size() - suffix.size());
  }
  sibling += ".share.md";
  ToolResult written =
      ToolWritePrivateFile(sibling, ShareMarkdown(*loaded.record));
  if (!written.Ok()) return {{"error", written.output}};
  return {{"shared", true}, {"path", sibling}};
}

SessionStoreStatus SessionStore::Rename(const std::string& path,
                                        const std::string& title) {
  if (!ValidSessionTitle(title)) {
    return Error(SessionStoreError::kInvalid,
                 "title must be 1–256 bytes without control characters");
  }
  FileLease writer;
  std::string error;
  if (!writer.Acquire(SessionLockPath(path), error)) {
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
  if (!writer.Acquire(SessionLockPath(path), error)) {
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
    ForEachString(facts, [&](const json& value) {
      const auto& text = value.get_ref<const std::string&>();
      if (ArtifactPath(text, artifacts)) {
        std::error_code ignored;
        std::filesystem::remove(
            artifacts / std::filesystem::path(text).filename(), ignored);
      }
    });
  }
  return ec ? Error(SessionStoreError::kIo, ec.message())
            : SessionStoreStatus{};
}

}  // namespace uagent
