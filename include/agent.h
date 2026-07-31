// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_H_
#define UAGENT_INCLUDE_AGENT_H_
// The agent loop. An Agent owns its message history and drives
// model -> tool -> model until the model answers in prose. Delegated work
// runs in a separate uagent process, so only its final result enters this
// conversation.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent/context_policy.h"
#include "include/agent/conversation.h"
#include "include/agent/dispatch.h"
#include "include/agent/protocol.h"
#include "include/agent/session_store.h"
#include "include/agent/trace.h"
#include "include/api.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/media.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"
#include "include/ui/conversation.h"
#include "third_party/json.hpp"

namespace uagent {

using nlohmann::json;

class Agent {
 public:
  // Asks the user to approve a mutating call; wired up by the host. Approving a
  // task authorizes its separate headless child for that scoped brief.
  using Approver = std::function<bool(const Tool&, const json& args)>;
  using ToolRefresher =
      std::function<bool(std::chrono::steady_clock::time_point)>;

  Agent(Api& api, std::vector<Tool>& tools, ProcessSupervisor& processes,
        UsageAccumulator& side_usage, Approver approve,
        ToolRefresher refresh_tools = {},
        ProjectInstructions project_instructions = {})
      : api_(api),
        tools_(tools),
        processes_(processes),
        side_usage_(side_usage),
        schemas_(ToolSchemas(tools)),
        approve_(std::move(approve)),
        refresh_tools_(std::move(refresh_tools)),
        project_instructions_(std::move(project_instructions)) {
    schema_chars_ = JsonDump(schemas_).size();
    if (g_debug.Enabled()) {
      json names = json::array();
      for (const Tool& tool : tools_) names.push_back(tool.name);
      g_debug.Write(
          "agent_init",
          {{"tools", std::move(names)},
           {"schemas", schemas_},
           {"schema_chars", schema_chars_},
           {"project_instruction_sources", project_instructions_.sources},
           {"project_instruction_chars", project_instructions_.text.size()},
           {"memory_sources", project_instructions_.memory_sources},
           {"memory_index_chars", project_instructions_.memory_index.size()},
           {"project_instructions_truncated",
            project_instructions_.truncated}});
    }
    Reset();
  }

  void Reset() {
    DebugLog("session_reset", {{"dropped_messages", conversation_.Size()},
                               {"prior_usage", UsageJson(session_usage_)}});
    conversation_.Reset(BaselineMessages(), BaselineKinds());
    turn_search_trace_.Reset();
    checkpoint_candidates_ = json::array();
    pending_checkpoint_ = nullptr;
    side_effects_ = json::array();
    session_usage_ = Usage{};
    context_policy_.Reset();
    logged_msgs_ = 0;
    total_user_turns_ = 0;
    session_title_.clear();
    session_id_ = MakeSessionId();
    last_checkpoint_turn_ = 0;
    g_image_input = true;
    ++revision_;
  }

  const Usage& SessionUsage() const { return session_usage_; }
  const std::string& LastError() const { return last_error_; }
  const std::string& SessionId() const { return session_id_; }
  size_t ArchivedSegments() const { return conversation_.ArchivedSegments(); }
  int64_t ArchivedBytes() const { return conversation_.ArchivedBytes(); }
  int64_t ArchiveDroppedSegments() const {
    return conversation_.DroppedSegments();
  }
  size_t CheckpointCandidates() const { return checkpoint_candidates_.size(); }
  uint64_t Revision() const { return revision_; }

  // Show the most recent completed turn's pruned tool traffic without keeping
  // a second transcript. Server search can expose sources and snippets, but
  // not necessarily the provider-internal query.
  void PrintTrace() const { PrintLatestTrace(conversation_.Archive(), tools_); }

  // final assistant prose — the whole result of a headless (-p) run
  std::string LastText() const { return conversation_.LastAssistantText(); }

  size_t MessageCount() const { return conversation_.UserVisibleCount(); }
  bool Verbose() const { return verbose_; }
  void SetVerbose(bool verbose) { verbose_ = verbose; }

  void RouteChanged() {
    context_policy_.SetReported(0);
    session_id_ = MakeSessionId();
    g_image_input = true;
    ++revision_;
  }

  // first user message, for the session picker's one-line title
  std::string FirstUserText() const {
    if (!session_title_.empty()) return session_title_;
    return conversation_.FirstUserText();
  }

  // Real user prompts are tracked out of band from model-readable text.
  int64_t UserTurns() const {
    if (total_user_turns_ > 0) return total_user_turns_;
    return conversation_.UserTurns();
  }

  // Replay the conversation to the terminal, in the live REPL's visual
  // language (user prompts, rendered assistant prose, dim tool traffic), so a
  // resumed session shows the context it is picking up from.
  void PrintHistory() const { PrintConversationHistory(conversation_, tools_); }

  bool Save(const std::string& path, std::string& error) const {
    SessionRecord record;
    record.metadata = {CanonicalCwd(), api_.model, session_id_, UserTurns(),
                       FirstUserText()};
    record.state = {conversation_.Messages(),
                    conversation_.Kinds(),
                    conversation_.Archive(),
                    conversation_.DroppedSegments(),
                    checkpoint_candidates_,
                    pending_checkpoint_,
                    side_effects_,
                    ContextUsed(),
                    session_usage_};
    SessionStoreStatus status = SessionStore::Save(path, record);
    if (!status.Ok()) {
      error = std::move(status.message);
      return false;
    }
    return true;
  }

  bool Load(const std::string& path, const std::string& expected_cwd,
            std::string& error) {
    SessionLoadResult loaded = SessionStore::Load(path, expected_cwd);
    if (!loaded.Ok()) {
      error = std::move(loaded.status.message);
      return false;
    }
    SessionRecord record = std::move(*loaded.record);
    if (!conversation_.Restore(std::move(record.state.messages),
                               std::move(record.state.message_kinds),
                               std::move(record.state.archive),
                               record.state.archive_dropped_segments)) {
      error = "session conversation state is invalid";
      return false;
    }
    RefreshBaseline();
    checkpoint_candidates_ = std::move(record.state.checkpoint_candidates);
    pending_checkpoint_ = std::move(record.state.pending_checkpoint);
    side_effects_ = std::move(record.state.side_effects);
    session_usage_ = record.state.usage;
    session_id_ = std::move(record.metadata.session_id);
    if (session_id_.empty()) session_id_ = MakeSessionId();
    total_user_turns_ = record.metadata.turns;
    session_title_ = std::move(record.metadata.title);
    context_policy_.Reset();
    context_policy_.SetReported(record.state.context_tokens);
    logged_msgs_ = 0;
    last_checkpoint_turn_ = 0;
    turn_search_trace_.Reset();
    ++revision_;
    return true;
  }

  // Conservative size of the next request: never below either the last
  // server-reported exchange or the current serialized prompt estimate.
  int64_t ContextUsed() const {
    return context_policy_.Used(JsonEstimatedBytes(conversation_.Messages()),
                                schema_chars_, api_.native_tools);
  }

  // summarize the conversation with the model, then restart the session
  // from that summary — frees the context without losing the thread
  bool Compact(bool automatic = false, Usage* turn_usage = nullptr) {
    if (MessageCount() < 2) {
      DebugLog("compact_skip", {{"reason", "empty"}, {"automatic", automatic}});
      printf("%s· nothing to compact%s\n", DIM(), RST());
      return false;
    }
    DebugLog("compact_start", {{"automatic", automatic},
                               {"messages", conversation_.Size()},
                               {"context_tokens", ContextUsed()}});
    printf("%s· %scompacting…%s\n", DIM(), automatic ? "auto-" : "", RST());
    size_t source_bytes = JsonEstimatedBytes(conversation_.Messages());
    size_t baseline_bytes = JsonEstimatedBytes(BaselineMessages());
    conversation_.Push(
        {{"role", "user"},
         {"content",
          "Summarize for a fresh context: goal, decisions, current state, "
          "relevant paths, and next steps. Be concise."}},
        MessageKind::kInternal);
    ChatResult r = Chat("compact", -1, json::array());
    conversation_.PopBack();  // never archive the summarization instruction
    Usage compact_usage;
    compact_usage.Add(r.usage);
    session_usage_.Merge(compact_usage);
    if (turn_usage) turn_usage->Merge(compact_usage);
    bool invalid_summary = r.content.empty() || !r.tool_calls.empty() ||
                           !ParseTextToolCalls(r.content).empty() ||
                           source_bytes <= baseline_bytes ||
                           r.content.size() >= source_bytes - baseline_bytes;
    if (r.interrupted || !r.error.empty() || invalid_summary) {
      std::string outcome =
          r.interrupted
              ? "interrupted"
              : (!r.error.empty()
                     ? "error"
                     : (r.content.empty() ? "empty" : "invalid_summary"));
      DebugLog(
          "compact_end",
          {{"automatic", automatic}, {"outcome", outcome}, {"error", r.error}});
      if (!r.error.empty()) {
        printf("%s%s%s\n", RED(), TerminalSafe(r.error).c_str(), RST());
      }
      return false;
    }
    PruneAttachments(BaselineSize());
    ArchiveAll(automatic ? "auto_compact" : "manual_compact");
    conversation_.ResetHistory(BaselineMessages(), BaselineKinds());
    conversation_.Push(
        {{"role", "assistant"},
         {"content",
          "[model-generated context summary; non-authoritative]\nPrior "
          "context:\n" +
              r.content}},
        MessageKind::kInternal);
    context_policy_.SetReported(0);
    context_policy_.ResetUrgency();
    ++revision_;
    DebugLog("compact_end", {{"automatic", automatic},
                             {"outcome", "ok"},
                             {"summary_chars", r.content.size()}});
    printf("\n%s· compacted%s\n", DIM(), RST());
    return true;
  }

  // Fold in what subagent processes spent. They bill against the same key, so
  // without this their cost is missing from the footer and the status bar.
  void DrainSubagentUsage() {
    std::string path = UsageLedger();
    std::string data;
    std::string error;
    if (!TakePrivateText(path, data, error)) {
      DebugLog("usage_ledger_error", {{"error", error}});
      return;
    }
    Usage spent;
    std::istringstream input(data);
    for (std::string line; std::getline(input, line);) {
      spent.Merge(UsageFromJson(json::parse(line, nullptr, false)));
    }
    side_usage_.Add(spent);
  }

  void MergeSideUsage(Usage& turn_usage) {
    DrainSubagentUsage();
    Usage spent = side_usage_.Take();
    turn_usage.Merge(spent);
    session_usage_.Merge(spent);
  }

  // Report finished background jobs to the user and hand them to the model.
  // The drain reaps and deletes each log, so its caller owns the only copy.
  void DrainBackground(TerminalSpinner* spinner = nullptr) {
    bool changed = false;
    for (auto& note : BgTakeCompleted(processes_)) {
      if (spinner) spinner->Stop();
      printf("%s· bg job finished %s%s\n", DIM(),
             TerminalSafe(FirstLine(note)).c_str(), RST());
      conversation_.Push({{"role", "user"}, {"content", std::move(note)}},
                         MessageKind::kInternal);
      changed = true;
    }
    if (DrainAttachments()) changed = true;
    if (changed) ++revision_;
  }

  // Files the model attached ride in on a user message: Chat Completions tool
  // results are text-only, so image/file parts cannot travel with them.
  bool DrainAttachments() {
    std::vector<Attachment> pending = g_attachments.Take();
    if (pending.empty()) return false;
    std::string error;
    json content = AttachmentContent("[attached on request]", pending, error);
    conversation_.Push(
        {{"role", "user"},
         {"content", error.empty() ? std::move(content)
                                   : json("[attachment failed] " + error)}},
        error.empty() ? MessageKind::kAttachment : MessageKind::kInternal);
    context_policy_.SetReported(0);
    DebugLog("attachments_added",
             {{"turn", turn_id_}, {"count", pending.size()}, {"error", error}});
    return true;
  }

  size_t JoinableBackground() const { return processes_.JoinableCount(); }

  bool WaitForBackground(std::chrono::steady_clock::time_point deadline,
                         Usage& usage) {
    DebugLog("background_join_start", {{"pending", JoinableBackground()}});
    SteeringGuard steering;
    TerminalSpinner spinner(false, SpinnerLabel("waiting for background"));
    while (!AbortRequested() && std::chrono::steady_clock::now() < deadline) {
      DrainBackground(&spinner);
      MergeSideUsage(usage);
      if (!JoinableBackground()) {
        spinner.Stop();
        DebugLog("background_join_end", {{"outcome", "complete"}});
        return true;
      }
      spinner.Start();
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      auto slice = std::min(remaining, std::chrono::milliseconds(100));
      std::this_thread::sleep_for(slice);
    }
    spinner.Stop();
    DebugLog("background_join_end",
             {{"outcome", AbortRequested() ? "interrupted" : "turn_timeout"},
              {"pending", JoinableBackground()}});
    return false;
  }

  bool JoinBackgroundOrReport(std::chrono::steady_clock::time_point deadline,
                              Usage& usage, int64_t max_turn_seconds,
                              std::string& outcome) {
    if (WaitForBackground(deadline, usage)) return true;
    BgCancelTasks(processes_);
    if (AbortRequested()) {
      ClearAbort();
      last_error_ = outcome = "interrupted";
    } else {
      last_error_ =
          "turn time limit reached (" + std::to_string(max_turn_seconds) + "s)";
      outcome = "budget_exceeded";
    }
    printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
    return false;
  }

  // one user turn: stream, run tools, repeat until prose; prints as it goes
  void Resume() {
    Turn(
        "(Continue the interrupted task from where you left off. Do not repeat "
        "completed "
        "work.)");
  }

  void Turn(const std::string& user_input, json user_content = nullptr);

 private:
  void ArchiveRange(const char* reason, size_t begin, size_t end,
                    json metadata = json::object()) {
    conversation_.ArchiveRange(reason, begin, end, turn_id_,
                               api_.config.session_archive_bytes,
                               std::move(metadata));
  }

  void ArchiveAll(const char* reason) {
    conversation_.ArchiveAll(reason, BaselineSize(), turn_id_,
                             api_.config.session_archive_bytes);
  }

  ChatResult Chat(const char* purpose, int64_t step, const json& schemas);

  std::string PrepareContext(size_t pending_chars);

  enum class MidturnCompact {
    kNotNeeded,
    kSucceeded,
    kFailed,
  };

  MidturnCompact MaybeCompactDuringTurn(const json& available_schemas,
                                        const std::string& active_prompt,
                                        Usage& usage, size_t& turn_start);

  // Encoded attachment bytes are never durable conversation state, including
  // on provider errors and interruption. Keep only a textual record.
  // Covers the whole turn, not just its first message: the model can attach
  // mid-turn, and those bytes are no more durable than the user's.
  void PruneAttachments(size_t turn_start);

  // A completed turn's final answer is the durable summary. Drop intermediate
  // calls/results (often entire files) so every future request stays lean.
  void PruneTurn(size_t turn_start);

  // A rejected capability -> drop it and retry. Ordered most-specific first:
  // the native-tools probe matches any "tool", so it must stay last or it would
  // swallow the parallel_tool_calls case. Image input is refused with a 404 on
  // some routers rather than a 400, so it is checked outside the 400 gate.
  bool DegradeAndRetry(const ChatResult& result);

  std::string SystemPrompt() const;

  // Message 0 is the one place the system shape is defined. Always rebuilt
  // rather than restored, so it tracks the current tools/protocol (see load()).
  json SysMsg() const;

  // Append environment state only when it changes. This preserves every prior
  // request byte for provider caching without repeating cwd metadata each turn.
  void EnsureEnvironmentContext();

  json ProjectInstructionMsg() const;
  json MemoryMsg() const;

  size_t BaselineSize() const;

  json BaselineMessages(bool checkpoint = false) const;

  std::vector<MessageKind> BaselineKinds() const;

  void RefreshBaseline();

  json CheckpointSysMsg() const;

  void AppendToolResult(const ToolCall& call, bool text_mode,
                        const std::string& result);
  std::vector<std::string> RecentToolResults(int64_t count) const;

  void InvalidatePendingCheckpoint(const char* reason);
  void RecordSideEffect(const CallTask& task, const ToolCall& call);
  void ApplyPendingCheckpoint();
  void ApplyCheckpoint(const std::string& state,
                       const std::vector<std::filesystem::path>& paths,
                       const std::vector<std::string>& results,
                       const std::vector<std::string>& verbatim);
  bool RunCheckpointCall(const ToolCall& call, bool text_mode,
                         int64_t& tool_count, int64_t step);

  // returns true if the user interrupted the batch
  bool RunCalls(const std::vector<ToolCall>& calls, bool text_mode,
                int64_t& tool_count,
                std::unordered_map<std::string, int64_t>& tool_counts,
                int64_t step, std::chrono::steady_clock::time_point deadline,
                int64_t& consecutive_failed_tools);

  void RebuildToolSchemas() {
    schemas_ = ToolSchemas(tools_);
    schema_chars_ = JsonDump(schemas_).size();
    if (!conversation_.Empty()) {
      conversation_.Set(0, SysMsg(), MessageKind::kSystem);
    }
    DebugLog("tool_registry_refreshed",
             {{"tools", tools_.size()}, {"schema_chars", schema_chars_}});
  }

  Api& api_;
  std::vector<Tool>& tools_;
  ProcessSupervisor& processes_;
  UsageAccumulator& side_usage_;
  json schemas_;  // request-shaped tool schemas, rebuilt after MCP changes
  size_t schema_chars_ = 0;
  Approver approve_;
  ToolRefresher refresh_tools_;
  ProjectInstructions project_instructions_;
  Conversation conversation_;
  ContextPolicy context_policy_;
  SearchTrace turn_search_trace_;
  json checkpoint_candidates_ = json::array();
  json pending_checkpoint_ = nullptr;
  json side_effects_ = json::array();
  Usage session_usage_;
  std::string session_id_;
  std::string session_title_;
  int64_t total_user_turns_ = 0;
  size_t logged_msgs_ = 0;  // messages already written to the debug trace
  int64_t turn_id_ = 0;
  int64_t request_id_ = 0;
  uint64_t revision_ = 0;
  int64_t last_checkpoint_turn_ = 0;
  bool checkpoint_hint_active_ = false;
  bool checkpoint_turn_complete_ = false;
  bool verbose_ = false;
  std::chrono::steady_clock::time_point active_deadline_ =
      std::chrono::steady_clock::time_point::max();
  std::string last_error_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_H_
