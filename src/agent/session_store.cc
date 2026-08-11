// Copyright 2026 Timon Gentzsch

#include "include/agent/session_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/files.h"

namespace uagent {
namespace {

constexpr int64_t kSessionFormat = 3;

SessionStoreStatus Error(SessionStoreError code, std::string message) {
  return {code, std::move(message)};
}

bool ValidState(const json& value) {
  return value.is_object() && value.contains("messages") &&
         value["messages"].is_array() && !value["messages"].empty() &&
         value.contains("message_kinds") && value["message_kinds"].is_array() &&
         value["message_kinds"].size() == value["messages"].size() &&
         value.contains("archive") && value["archive"].is_array() &&
         value.contains("archive_dropped_segments") &&
         value["archive_dropped_segments"].is_number_integer() &&
         value.contains("context_tokens") &&
         value["context_tokens"].is_number_integer() &&
         value.contains("usage") && value["usage"].is_object();
}

bool ValidHeader(const json& header) {
  return header.is_object() && header.contains("cwd") &&
         header["cwd"].is_string() && header.contains("model") &&
         header["model"].is_string() && header.contains("session_id") &&
         header["session_id"].is_string() && header.contains("turns") &&
         header["turns"].is_number_integer() && header.contains("title") &&
         header["title"].is_string();
}

json HeaderJson(const SessionMetadata& metadata) {
  return {{"format", kSessionFormat}, {"cwd", metadata.cwd},
          {"model", metadata.model},  {"session_id", metadata.session_id},
          {"turns", metadata.turns},  {"title", metadata.title}};
}

json StateJson(const SessionState& state) {
  return {{"messages", state.messages},
          {"message_kinds", MessageKindsJson(state.message_kinds)},
          {"archive", state.archive},
          {"archive_dropped_segments", state.archive_dropped_segments},
          {"context_tokens", state.context_tokens},
          {"usage", UsageJson(state.usage)},
          {"route_usage", RouteUsageJson(state.route_usage)}};
}

}  // namespace

SessionStoreStatus SessionStore::Save(const std::string& path,
                                      const SessionRecord& record) {
  json header = HeaderJson(record.metadata);
  json state = StateJson(record.state);
  if (!ValidHeader(header) || !ValidState(state)) {
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
      header["cwd"].get<std::string>(), saved_error);
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
  record.metadata.cwd = header["cwd"].get<std::string>();
  record.metadata.model = header["model"].get<std::string>();
  record.metadata.session_id = header["session_id"].get<std::string>();
  record.metadata.turns = header["turns"].get<int64_t>();
  record.metadata.title = header["title"].get<std::string>();
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
  return {{}, std::move(record)};
}

}  // namespace uagent
