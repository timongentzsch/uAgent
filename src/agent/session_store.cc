// Copyright 2026 Timon Gentzsch

#include "include/agent/session_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/files.h"

namespace uagent {
namespace {

constexpr int64_t kSessionFormat = 2;

SessionStoreStatus Error(SessionStoreError code, std::string message) {
  return {code, std::move(message)};
}

bool ValidState(const json& value) {
  return value.is_object() && value.contains("messages") &&
         value["messages"].is_array() && !value["messages"].empty() &&
         value.contains("archive") && value["archive"].is_array() &&
         value.contains("archive_dropped_segments") &&
         value["archive_dropped_segments"].is_number_integer() &&
         value.contains("checkpoint_candidates") &&
         value["checkpoint_candidates"].is_array() &&
         value.contains("pending_checkpoint") &&
         (value["pending_checkpoint"].is_null() ||
          value["pending_checkpoint"].is_object()) &&
         value.contains("side_effects") && value["side_effects"].is_array() &&
         value.contains("context_tokens") &&
         value["context_tokens"].is_number_integer() &&
         value.contains("usage") && value["usage"].is_object();
}

json HeaderJson(const SessionMetadata& metadata) {
  return {{"format", kSessionFormat}, {"cwd", metadata.cwd},
          {"model", metadata.model},  {"session_id", metadata.session_id},
          {"turns", metadata.turns},  {"title", metadata.title}};
}

json StateJson(const SessionState& state) {
  return {{"messages", state.messages},
          {"archive", state.archive},
          {"archive_dropped_segments", state.archive_dropped_segments},
          {"checkpoint_candidates", state.checkpoint_candidates},
          {"pending_checkpoint", state.pending_checkpoint},
          {"side_effects", state.side_effects},
          {"context_tokens", state.context_tokens},
          {"usage", UsageJson(state.usage)}};
}

}  // namespace

SessionStoreStatus SessionStore::Save(const std::string& path,
                                      const SessionRecord& record) {
  ToolResult result =
      ToolWritePrivateFile(path, JsonDump(HeaderJson(record.metadata)) + "\n" +
                                     JsonDump(StateJson(record.state)));
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
  body = Trim(std::move(body));
  if (body.empty()) {
    return {Error(SessionStoreError::kCorrupt, "session is incomplete"),
            std::nullopt};
  }

  json header = json::parse(header_line, nullptr, false);
  if (!header.is_object() ||
      JsonValue(header, "format", int64_t{0}) != kSessionFormat ||
      !header.contains("cwd") || !header["cwd"].is_string() ||
      !header.contains("model") || !header["model"].is_string() ||
      !header.contains("session_id") || !header["session_id"].is_string() ||
      !header.contains("turns") || !header["turns"].is_number_integer() ||
      !header.contains("title") || !header["title"].is_string()) {
    return {Error(SessionStoreError::kCorrupt,
                  "session header is invalid or unsupported"),
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

  SessionRecord record;
  record.metadata.cwd = header["cwd"].get<std::string>();
  record.metadata.model = header["model"].get<std::string>();
  record.metadata.session_id = header["session_id"].get<std::string>();
  record.metadata.turns = header["turns"].get<int64_t>();
  record.metadata.title = header["title"].get<std::string>();
  record.state.messages = std::move(state["messages"]);
  record.state.archive = std::move(state["archive"]);
  record.state.archive_dropped_segments =
      std::max(int64_t{0}, state["archive_dropped_segments"].get<int64_t>());
  record.state.checkpoint_candidates =
      std::move(state["checkpoint_candidates"]);
  record.state.pending_checkpoint = std::move(state["pending_checkpoint"]);
  record.state.side_effects = std::move(state["side_effects"]);
  record.state.context_tokens =
      std::max(int64_t{0}, state["context_tokens"].get<int64_t>());
  record.state.usage = UsageFromJson(state["usage"]);
  return {{}, std::move(record)};
}

}  // namespace uagent
