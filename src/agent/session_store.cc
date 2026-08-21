// Copyright 2026 Timon Gentzsch

#include "include/agent/session_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/adaptive_system.h"
#include "include/core/json.h"
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
    {"adaptive_system_revision", json::value_t::number_unsigned, false}};

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
          {kSessionHeaderTitle, metadata.title}};
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
          {"adaptive_system_revision", state.adaptive_system_revision}};
}

}  // namespace

SessionStoreStatus SessionStore::Save(const std::string& path,
                                      const SessionRecord& record) {
  json header = HeaderJson(record.metadata);
  json state = StateJson(record.state);
  // The header is built from a typed struct; only the state can be incomplete.
  if (!ValidState(state)) {
    return Error(SessionStoreError::kInvalid,
                 "refusing to save incomplete session state");
  }
  ToolResult result =
      ToolWritePrivateFile(path, JsonDump(header) + "\n" + JsonDump(state));
  if (!result.Ok()) return Error(SessionStoreError::kIo, result.output);
  return {};
}

SessionLoadResult SessionStore::Load(const std::string& path,
                                     const std::string& expected_cwd) {
  std::ifstream input(path);
  if (!input) {
    return {Error(SessionStoreError::kNotFound, "cannot open session"),
            std::nullopt};
  }

  std::string header_line;
  if (!std::getline(input, header_line)) {
    return {Error(SessionStoreError::kCorrupt, "session is incomplete"),
            std::nullopt};
  }
  std::string body{std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>()};
  body = Trim(body);
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

  std::error_code saved_error;
  std::error_code expected_error;
  std::filesystem::path saved = std::filesystem::weakly_canonical(
      header[kSessionHeaderCwd].get<std::string>(), saved_error);
  std::filesystem::path expected =
      std::filesystem::weakly_canonical(expected_cwd, expected_error);
  if (saved_error || expected_error) {
    return {
        Error(SessionStoreError::kCorrupt, "session workspace path is invalid"),
        std::nullopt};
  }
  if (saved != expected) {
    return {Error(SessionStoreError::kWrongWorkspace,
                  "session belongs to " + saved.string() + ", not " +
                      expected.string()),
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
  return {{}, std::move(record)};
}

}  // namespace uagent
