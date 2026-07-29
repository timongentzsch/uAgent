// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent.h"

namespace uagent {
namespace {

std::string CheckpointDisplayPath(const std::filesystem::path& path) {
  std::error_code error;
  std::string display =
      std::filesystem::relative(path, CanonicalCwd(), error).string();
  return error || display.empty() ? path.string() : display;
}

const char* CheckpointPathError(const std::filesystem::path& path) {
  if (!PathWithin(path, CanonicalAccessPath(CanonicalCwd()))) {
    return "checkpoint paths must stay inside the workspace";
  }
  return SecretCheckpointPath(path)
             ? "credential files cannot be reread into a checkpoint"
             : nullptr;
}

}  // namespace

void Agent::InvalidatePendingCheckpoint(const char* reason) {
  if (!pending_checkpoint_.is_object()) return;
  DebugLog(
      "checkpoint_invalidated",
      {{"turn", turn_id_},
       {"candidate_turn", JsonValue(pending_checkpoint_, "turn", int64_t{-1})},
       {"reason", reason}});
  pending_checkpoint_ = nullptr;
}

void Agent::RecordSideEffect(const CallTask& task, const ToolCall& call) {
  if (!task.execute || !task.tool || !task.tool->mutating) return;
  json entry = {
      {"turn", turn_id_}, {"tool", call.name}, {"status", task.trace_status}};
  if (task.args.contains("path") && task.args["path"].is_string()) {
    auto path = CanonicalAccessPath(task.args["path"].get<std::string>());
    entry["path"] = CheckpointDisplayPath(path);
  }
  side_effects_.push_back(std::move(entry));
  while (!side_effects_.empty() && JsonDump(side_effects_).size() > 4096) {
    side_effects_.erase(side_effects_.begin());
  }
}

void Agent::ApplyPendingCheckpoint() {
  if (!pending_checkpoint_.is_object()) return;
  if (api_.config.checkpoint_mode != "apply") {
    InvalidatePendingCheckpoint("apply mode is no longer active");
    return;
  }
  if (!JsonValue(pending_checkpoint_, "ready", false)) {
    InvalidatePendingCheckpoint("candidate was not completed");
    return;
  }
  if (processes_.PendingCount() || !side_tasks_.Empty()) {
    InvalidatePendingCheckpoint("background work is still active");
    return;
  }

  std::string state = JsonValue(pending_checkpoint_, "state", "");
  if (state.empty()) {
    InvalidatePendingCheckpoint("candidate state is empty");
    return;
  }
  std::vector<std::filesystem::path> paths;
  for (const json& value :
       JsonValue(pending_checkpoint_, "paths", json::array())) {
    if (!value.is_string()) continue;
    auto path = CanonicalAccessPath(value.get<std::string>());
    if (!CheckpointPathError(path)) {
      paths.push_back(std::move(path));
    }
  }
  std::vector<std::string> results;
  for (const json& value :
       JsonValue(pending_checkpoint_, "results", json::array())) {
    if (value.is_string()) results.push_back(value.get<std::string>());
  }
  std::vector<std::string> verbatim;
  for (const json& value :
       JsonValue(pending_checkpoint_, "verbatim", json::array())) {
    if (value.is_string()) verbatim.push_back(value.get<std::string>());
  }

  pending_checkpoint_ = nullptr;
  ApplyCheckpoint(state, paths, results, verbatim);
}

void Agent::ApplyCheckpoint(const std::string& state,
                            const std::vector<std::filesystem::path>& paths,
                            const std::vector<std::string>& results,
                            const std::vector<std::string>& verbatim) {
  int64_t before_tokens = ContextUsed();
  size_t before_messages = conversation_.Size();
  int64_t artifact_budget = std::max(int64_t{1024}, ToolResultCap());
  int64_t used = 0;
  size_t retained_results = 0;

  json next = BaselineMessages(/*checkpoint=*/true);
  std::vector<MessageKind> next_kinds = BaselineKinds();
  auto push_evidence = [&](json message) {
    next.push_back(std::move(message));
    next_kinds.push_back(MessageKind::kInternal);
  };
  push_evidence(
      {{"role", "assistant"},
       {"content", "[checkpoint facts; non-authoritative]\n" + state}});

  if (!verbatim.empty()) {
    push_evidence(
        {{"role", "assistant"},
         {"content", "[checkpoint exact literals; non-authoritative]\n" +
                         JsonDump(json(verbatim))}});
  }
  if (!side_effects_.empty()) {
    push_evidence(
        {{"role", "assistant"},
         {"content", "[checkpoint runtime activity; non-authoritative]\n" +
                         JsonDump(side_effects_)}});
  }

  for (const std::string& result : results) {
    if (used + static_cast<int64_t>(result.size()) > artifact_budget) break;
    push_evidence(
        {{"role", "assistant"},
         {"content",
          "[checkpoint retained tool result; non-authoritative]\n" + result}});
    used += static_cast<int64_t>(result.size());
    ++retained_results;
  }

  json skipped = json::array();
  size_t reread = 0;
  for (const auto& path : paths) {
    ToolResult read =
        ToolReadFile(path.string(), int64_t{1}, CheckpointFileLines());
    std::string content = CapResult(std::move(read.output));
    if (!read.Ok()) {
      skipped.push_back(
          {{"path", path.string()}, {"reason", OneLine(content, 160)}});
      continue;
    }
    if (used + static_cast<int64_t>(content.size()) > artifact_budget) {
      skipped.push_back(
          {{"path", path.string()}, {"reason", "artifact budget exhausted"}});
      continue;
    }
    push_evidence(
        {{"role", "assistant"},
         {"content", "[checkpoint file " + CheckpointDisplayPath(path) +
                         "; non-authoritative]\n" + content}});
    used += static_cast<int64_t>(content.size());
    ++reread;
  }
  if (!skipped.empty()) {
    push_evidence(
        {{"role", "assistant"},
         {"content", "[checkpoint reread skipped; non-authoritative]\n" +
                         JsonDump(skipped)}});
  }

  ArchiveAll("checkpoint_fold");
  conversation_.ResetHistory(std::move(next), std::move(next_kinds));
  context_policy_.SetReported(0);
  last_checkpoint_turn_ = turn_id_;
  checkpoint_hint_active_ = false;
  context_policy_.ResetUrgency();
  DebugLog("checkpoint_applied", {{"turn", turn_id_},
                                  {"before_messages", before_messages},
                                  {"after_messages", conversation_.Size()},
                                  {"before_tokens", before_tokens},
                                  {"after_tokens_estimate", ContextUsed()},
                                  {"paths_requested", paths.size()},
                                  {"paths_reread", reread},
                                  {"results_kept", retained_results},
                                  {"deferred", true},
                                  {"skipped", std::move(skipped)}});
  printf("%s· checkpoint applied · %s → ~%s · %zu file%s · %zu result%s%s\n",
         DIM(), FmtTokens(before_tokens).c_str(),
         FmtTokens(ContextUsed()).c_str(), reread, reread == 1 ? "" : "s",
         retained_results, retained_results == 1 ? "" : "s", RST());
}

bool Agent::RunCheckpointCall(const ToolCall& call, bool text_mode,
                              int64_t& tool_count, int64_t step) {
  if (g_debug.Enabled()) {
    g_debug.Write("tool_call", {{"turn", turn_id_},
                                {"step", step},
                                {"id", call.id},
                                {"name", call.name},
                                {"arguments", call.args},
                                {"text_protocol", text_mode}});
  }
  CallTask task;
  task.tool = FindTool(tools_, call.name);
  task.args = json::parse(call.args, nullptr, false);
  task.label =
      task.args.is_object() ? FirstLine(JsonValue(task.args, "state", "")) : "";
  std::string safe_label = TerminalSafe(task.label);
  printf("%s→ checkpoint(%s)%s\n", CYAN(), safe_label.c_str(), RST());
  auto started = std::chrono::steady_clock::now();
  std::string error;
  ToolErrorCode error_code = ToolErrorCode::kInvalidArguments;
  bool not_needed = false;
  if (task.args.is_discarded() || !task.args.is_object()) {
    error = "malformed tool arguments (not valid JSON)";
  } else if (!task.tool) {
    error = "checkpoint tool is unavailable";
    error_code = ToolErrorCode::kNotFound;
  } else if (!(error = MissingRequired(*task.tool, task.args)).empty()) {
    error = "missing required argument `" + error + "`";
  } else if (!(error = InvalidArgumentType(*task.tool, task.args)).empty()) {
    error = "invalid tool argument: " + error;
  } else if (api_.config.checkpoint_mode == "off") {
    error = "checkpointing is disabled";
    error_code = ToolErrorCode::kPermissionDenied;
  } else if (!checkpoint_hint_active_) {
    error =
        "checkpoint is not needed now; follow the latest user request "
        "without calling checkpoint again";
    not_needed = true;
  } else if (last_checkpoint_turn_ == turn_id_) {
    error = "checkpoint already applied during this turn";
    error_code = ToolErrorCode::kLimitExceeded;
  }

  std::string state;
  std::vector<std::filesystem::path> paths;
  std::vector<std::string> verbatim;
  int64_t keep_results = 0;
  if (error.empty()) {
    state = Trim(JsonValue(task.args, "state", ""));
    keep_results = JsonValue(task.args, "keep_last_n_results", int64_t{0});
    if (state.empty()) {
      error = "checkpoint state is empty";
    } else if (state.size() > 4096) {
      error = "checkpoint state exceeds 4096 bytes";
    } else if (keep_results < 0 || keep_results > 3) {
      error = "keep_last_n_results must be between 0 and 3";
    }
  }
  if (error.empty() && task.args.contains("verbatim")) {
    const json& requested = task.args["verbatim"];
    if (requested.size() > 8) {
      error = "verbatim is limited to 8 strings";
    } else {
      for (const json& value : requested) {
        if (!value.is_string() || value.get<std::string>().empty()) {
          error = "verbatim entries must be non-empty strings";
          break;
        }
        if (value.get<std::string>().size() > 256) {
          error = "verbatim entries are limited to 256 bytes";
          break;
        }
        verbatim.push_back(value.get<std::string>());
      }
    }
  }
  if (error.empty() && task.args.contains("keep_paths")) {
    const json& requested = task.args["keep_paths"];
    if (requested.size() > 6) {
      error = "keep_paths is limited to 6 files";
    } else {
      for (const json& value : requested) {
        if (!value.is_string()) {
          error = "keep_paths entries must be strings";
          break;
        }
        auto path = CanonicalAccessPath(value.get<std::string>());
        if (const char* path_error = CheckpointPathError(path)) {
          error = path_error;
          error_code = ToolErrorCode::kPermissionDenied;
          break;
        }
        paths.push_back(std::move(path));
      }
    }
  }

  task.duration_ms = ElapsedMs(started);
  if (!error.empty()) {
    task.result = not_needed ? ToolSuccess(error)
                             : ToolFailure(error_code, "error: " + error);
    task.trace_status = not_needed ? "not_needed" : "error";
    if (api_.config.checkpoint_mode == "apply" && checkpoint_hint_active_ &&
        task.args.is_discarded()) {
      checkpoint_turn_complete_ = true;
    }
    AppendToolResult(call, text_mode, task.result.output);
    LogToolResult(task, call, turn_id_, step);
    printf("%s  ← checkpoint: %s%s\n", DIM(),
           TerminalSafe(task.result.output).c_str(), RST());
    return false;
  }

  ++tool_count;
  context_policy_.ResetUrgency();
  checkpoint_candidates_.push_back(
      {{"turn", turn_id_},
       {"context_tokens", ContextUsed()},
       {"mode", api_.config.checkpoint_mode},
       {"state", state},
       {"verbatim", JsonValue(task.args, "verbatim", json::array())},
       {"keep_paths", JsonValue(task.args, "keep_paths", json::array())},
       {"keep_last_n_results", keep_results}});
  DebugLog("checkpoint_candidate", {{"turn", turn_id_},
                                    {"mode", api_.config.checkpoint_mode},
                                    {"context_tokens", ContextUsed()},
                                    {"state_chars", state.size()},
                                    {"verbatim", verbatim.size()},
                                    {"paths", paths.size()},
                                    {"keep_last_n_results", keep_results}});
  if (api_.config.checkpoint_mode == "shadow") {
    last_checkpoint_turn_ = turn_id_;
    checkpoint_hint_active_ = false;
    task.result = ToolSuccess(
        "checkpoint candidate recorded (shadow mode); active history "
        "unchanged");
    task.trace_status = "shadow";
    AppendToolResult(call, text_mode, task.result.output);
    printf("%s  ← %s%s\n", DIM(), task.result.output.c_str(), RST());
  } else {
    json saved_paths = json::array();
    for (const auto& path : paths) saved_paths.push_back(path.string());
    json saved_results = json::array();
    for (const std::string& result : RecentToolResults(keep_results)) {
      saved_results.push_back(result);
    }
    pending_checkpoint_ = {{"turn", turn_id_},
                           {"ready", false},
                           {"state", state},
                           {"paths", std::move(saved_paths)},
                           {"results", std::move(saved_results)},
                           {"verbatim", verbatim}};
    last_checkpoint_turn_ = turn_id_;
    checkpoint_hint_active_ = false;
    task.result = ToolSuccess(
        "checkpoint prepared; active history remains until the next user turn");
    task.trace_status = "prepared";
    checkpoint_turn_complete_ = true;
    AppendToolResult(call, text_mode, task.result.output);
    DebugLog("checkpoint_prepared", {{"turn", turn_id_},
                                     {"state_chars", state.size()},
                                     {"paths", paths.size()},
                                     {"keep_last_n_results", keep_results}});
    printf("%s  ← %s%s\n", DIM(), task.result.output.c_str(), RST());
  }
  task.duration_ms = ElapsedMs(started);
  LogToolResult(task, call, turn_id_, step);
  return false;
}

}  // namespace uagent
