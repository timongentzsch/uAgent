// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_H_
#define UAGENT_INCLUDE_AGENT_H_
// The agent loop. An Agent owns its message history and drives
// model -> tool -> model until the model answers in prose. Delegated work
// runs in a separate uagent process, so only its final result enters this
// conversation.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/agent/adaptive_system.h"
#include "include/agent/conversation.h"
#include "include/agent/trace.h"
#include "include/api.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/skills.h"
#include "include/core/usage.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

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
        ProjectInstructions project_instructions = {},
        std::vector<Skill> skills = {},
        AdaptiveSystemState* adaptive_system = nullptr);

  void Reset();

  const Usage& SessionUsage() const { return session_usage_; }
  json RouteUsageJson() const { return uagent::RouteUsageJson(route_usage_); }
  const std::string& LastError() const { return last_error_; }
  const std::string& SessionId() const { return session_id_; }
  uint64_t Revision() const { return revision_; }

  // Show the most recent completed turn's archived tool traffic. Server search
  // can expose sources and snippets, but not necessarily the provider-internal
  // query.
  void PrintTrace() const;

  json LatestToolTrace() const;

  // final assistant prose — the whole result of a headless (-p) run
  std::string LastText() const { return conversation_.LastAssistantText(); }

  size_t MessageCount() const { return conversation_.UserVisibleCount(); }
  bool Verbose() const { return verbose_; }
  void SetVerbose(bool verbose) { verbose_ = verbose; }

  void RouteChanged();

  std::string ActiveRoute() const;

  // Session picker's one-line title.
  std::string FirstUserText() const;

  // Real user prompts are tracked out of band from model-readable text.
  int64_t UserTurns() const;

  // Replay the conversation to the terminal, in the live REPL's visual
  // language (user prompts, rendered assistant prose, dim tool traffic), so a
  // resumed session shows the context it is picking up from.
  void PrintHistory() const;

  void PrintContext() const;

  bool Save(const std::string& path, std::string& error) const;

  bool Load(const std::string& path, const std::string& expected_cwd,
            std::string& error);

  // Estimated tokens in the request currently represented by the conversation.
  // Provider usage belongs to billing and may be cumulative or stale.
  int64_t ContextUsed() const;
  int64_t ContextSnapshot() const {
    return context_snapshot_.load(std::memory_order_relaxed);
  }

  // summarize the conversation with the model, then restart the session
  // from that summary — frees the context without losing the thread
  bool Compact(bool automatic = false, Usage* turn_usage = nullptr);

  // Fold in what subagent processes spent. They bill against the same key, so
  // without this their cost is missing from the footer and the status bar.
  void DrainSubagentUsage();

  void MergeSideUsage(Usage& turn_usage);

  void MergeSessionUsage(const Usage& usage);

  // Report finished background jobs to the user and hand them to the model.
  // The drain reaps and deletes each log, so its caller owns the only copy.
  bool DrainBackground();

  // Files the model attached ride in on a user message: Chat Completions tool
  // results are text-only, so image/file parts cannot travel with them.
  bool DrainAttachments();

  // Start a harness-origin turn after background results were delivered. This
  // is not user intent and must not advance user-turn bookkeeping.
  void ContinueAfterActivity();

  // one user turn: stream, run tools, repeat until prose; prints as it goes
  void Turn(const std::string& user_input, json user_content = nullptr);

 private:
  struct TurnState;

  enum class ImageFallbackCause { kKnownUnsupported, kRejected };
  struct ImageFallbackResult {
    bool applied = false;
    bool warning = false;
    size_t rewritten = 0;
    std::string error;
    std::string status;
  };

  void RunTurn(const std::string& input, json content, bool harness_origin);

  std::string AnalyzeImageContent(const json& content, std::string& error);
  ImageFallbackResult ApplyImageAnalysisFallback(json& messages,
                                                 ImageFallbackCause cause);
  static void ReportImageFallback(const ImageFallbackResult& result);
  // Run the fallback over one user message's content parts and report it.
  ImageFallbackResult ApplyImageFallbackToUserContent(json& content);

  void AddRouteUsage(const Usage& usage);
  Usage AccountModelUsage(const json& reported);

  void FailBudget(TurnState& state, std::string message);
  bool TurnDeadlineExceeded(TurnState& state,
                            std::chrono::seconds reserve = {});
  bool TurnCostExceeded(TurnState& state);
  void RecordModelResponse(
      ChatResult& response, TurnState& state,
      std::unordered_map<std::string, int64_t>& tool_counts);
  bool ToolCallsWithinLimits(const std::vector<ToolCall>& calls,
                             TurnState& state, int64_t max_tool_calls,
                             std::string& last_call, int64_t& repeated_calls);
  void FinishTurn(TurnState& state, int64_t step);
  static std::string TurnStatsLine(const TurnState& state, double seconds,
                                   double tokens_per_second);

  void ArchiveAll(const char* reason);

  ChatResult Chat(const char* purpose, int64_t step, const json& schemas,
                  bool render_output = true);

  size_t RequestContextBytes(size_t schema_bytes) const;
  int64_t SnapshotContext(size_t schema_bytes) const;
  int64_t ContextPressurePct(size_t pending_bytes, size_t schema_bytes,
                             int64_t* projected_tokens = nullptr) const;
  bool ContextNeedsCompaction(size_t pending_bytes, size_t schema_bytes,
                              int64_t& pressure,
                              int64_t& projected_tokens) const;

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

  // Keep completed tool messages in active context until normal compaction,
  // while also archiving them for the user-facing /trace command.
  void ArchiveTurnTrace(size_t turn_start);
  void PruneOldToolResults();

  // A rejected capability -> drop it and retry. Ordered most-specific first:
  // the native-tools probe matches any "tool", so it must stay last or it would
  // swallow the parallel_tool_calls case. Image input is refused with a 404 on
  // some routers rather than a 400, so it is checked outside the 400 gate.
  bool DegradeAndRetry(const ChatResult& result);

  std::string SystemPrompt() const;
  void RefreshSystemMessage();

  // Message 0 is the one place the system shape is defined. Always rebuilt
  // rather than restored, so it tracks the current tools/protocol (see load()).
  json SysMsg() const;
  void EnsureRuntimeContext();

  // Append environment state only when it changes. This preserves every prior
  // request byte for provider caching without repeating cwd metadata each turn.

  json ProjectInstructionMsg() const;
  json MemoryMsg() const;

  size_t BaselineSize() const;

  json BaselineMessages() const;

  std::vector<MessageKind> BaselineKinds() const;

  void RefreshBaseline();

  void AppendToolResult(const ToolCall& call, bool text_mode,
                        const std::string& result);

  // returns true if the user interrupted the batch
  bool RunCalls(const std::vector<ToolCall>& calls, bool text_mode,
                int64_t& tool_count,
                std::unordered_map<std::string, int64_t>& tool_counts,
                std::unordered_map<std::string, std::string>& stable_arguments,
                int64_t step, std::chrono::steady_clock::time_point deadline,
                int64_t& consecutive_failed_tools);

  void RebuildToolSchemas();
  std::vector<std::string> ExplicitSkillContext(
      const std::string& user_input) const;

  Api& api_;
  std::vector<Tool>& tools_;
  ProcessSupervisor& processes_;
  UsageAccumulator& side_usage_;
  json schemas_;  // request-shaped tool schemas, rebuilt after MCP changes
  size_t schema_chars_ = 0;
  Approver approve_;
  ToolRefresher refresh_tools_;
  ProjectInstructions project_instructions_;
  std::vector<Skill> skills_;
  AdaptiveSystemState* adaptive_system_ = nullptr;
  uint64_t applied_system_revision_ = 0;
  Conversation conversation_;
  mutable std::atomic<int64_t> context_snapshot_{0};
  SearchTrace turn_search_trace_;
  Usage session_usage_;
  RouteUsage route_usage_;
  std::string session_id_;
  std::string session_title_;
  int64_t total_user_turns_ = 0;
  size_t logged_msgs_ = 0;  // messages already written to the debug trace
  int64_t turn_id_ = 0;
  int64_t request_id_ = 0;
  uint64_t revision_ = 0;
  bool verbose_ = false;
  bool cost_warning_shown_ = false;
  std::chrono::steady_clock::time_point active_deadline_ =
      std::chrono::steady_clock::time_point::max();
  std::string last_error_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_H_
