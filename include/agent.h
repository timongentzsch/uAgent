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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent/adaptive_system.h"
#include "include/agent/conversation.h"
#include "include/agent/edit_journal.h"
#include "include/agent/process.h"
#include "include/agent/trace.h"
#include "include/api.h"
#include "include/core/json.h"
#include "include/core/lease.h"
#include "include/core/project.h"
#include "include/core/skills.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/tool.h"

namespace uagent {
struct Mail;

struct BackgroundCompletion;
struct CallTask;
enum class TurnStopReason;
enum class Fault;

class Agent {
 public:
  // Asks the user to approve a mutating call; wired up by the host. Returns
  // why it was refused, empty when it may run, so the model learns who said
  // no. Approving a task authorizes its separate headless child for that
  // scoped brief.
  using Approver =
      std::function<std::string(const Tool&, const json& args, int64_t turn)>;
  using ToolRefresher =
      std::function<bool(std::chrono::steady_clock::time_point)>;

  Agent(Api& api, std::vector<Tool>& tools, ProcessSupervisor& processes,
        UsageAccumulator& side_usage, Approver approve,
        ToolRefresher refresh_tools = {},
        ProjectInstructions project_instructions = {},
        std::vector<Skill> skills = {},
        AdaptiveSystemState* adaptive_system = nullptr);

  void Reset();
  // What the model reads now, with its sources.
  json PromptPreview() const;
  // The conversation's self-directive (adapt_system): show, preview, set,
  // edit (one exact replacement) or reset, guarded by its revision.
  json SelfDirective(const json& request);
  const std::string& LastSentPrompt() const { return last_sent_prompt_; }

  const Usage& SessionUsage() const { return session_usage_; }
  json RouteUsageJson() const { return uagent::RouteUsageJson(route_usage_); }
  const std::string& LastError() const { return last_error_; }
  // Why the last turn ended, in a vocabulary a caller can branch on, with the
  // limits that were in force. Empty before the first turn completes.
  const json& LastStop() const { return last_stop_; }
  const std::string& SessionId() const { return session_id_; }
  uint64_t Revision() const { return revision_; }
  // The most recent completed turn's archived tool traffic as data. Server
  // search can expose sources and snippets, but not necessarily the
  // provider-internal query. Consumer interfaces render it; the agent never
  // prints.
  json LatestToolTrace() const;

  // final assistant prose — the whole result of a headless (-p) run
  std::string LastText() const { return conversation_.LastAssistantText(); }

  size_t MessageCount() const { return conversation_.Size(); }

  void RouteChanged();

  // Approval mode is host-owned but can change between interactive turns.
  // Rebuild message zero after the host updates its canonical environment.

  std::string ActiveRoute() const;

  // Vision route for the analysis side call: the configured model wins;
  // otherwise the main route reads images when it can, else the shared
  // flash default (vision-capable) backs it. Empty means inherit main.
  std::string EffectiveImageModel() const;

  // Session picker's one-line title.
  std::string FirstUserText() const;

  // Real user prompts are tracked out of band from model-readable text.
  int64_t UserTurns() const;

  // Data views for consumer interfaces (terminal, web, headless). Rendering
  // lives in ui/; the agent only supplies facts.
  const Conversation& History() const { return conversation_; }
  json DisplaySnapshot() const;
  // Rewinds this conversation in place to just before a user message (by
  // `turn`, or by its "m-<id>"), returning {prompt} with that message's text,
  // or {error}. Bumps ViewEpoch: holders of the old view must replace it.
  json RewindBefore(int64_t turn, const std::string& message_id);
  uint64_t ViewEpoch() const { return view_epoch_; }
  // /btw: one tool-less model call over the conversation as of the last
  // request or turn end, never recorded. Safe beside a running turn.
  json SideQuestion(const std::string& question) const;
  // Snapshot the conversation (and the tools last offered, when given) for
  // SideQuestion; called by the turn thread only.
  void PublishSideContext(const json* tools = nullptr);
  // Snapshots a file a tool added to context where the user's clients can
  // show it, returning its asset ({id, name, mime, bytes}) or null. Set by a
  // session with an asset store; without one, tool files stay model-only.
  using KeepFile =
      std::function<json(const std::string& path, const std::string& name)>;
  void KeepToolFiles(KeepFile keep) { keep_tool_file_ = std::move(keep); }
  // Coordinator/thread role ({kind, thread}), saved in the session header.
  void SetSessionRole(json role) { session_role_ = std::move(role); }
  // Extra context rebuilt for every model request (a coordinator's clock,
  // pinned notes and board): appended to the request, never stored.
  void SetRuntimeContext(std::function<std::string()> extra) {
    runtime_context_ = std::move(extra);
  }
  void RetainExchanges(bool enabled) {
    retain_exchanges_ = enabled;
    api_.capture_http = enabled;
  }
  json HttpExchanges() const;
  json SessionSettings() const;
  void SessionSettings(const json& settings);
  json ToolCatalogue() const;
  // The tools this conversation has switched on, by name: the most a child
  // it delegates to may be given.
  std::vector<std::string> EnabledTools() const;
  json ConfigureTools(const json& request);
  json ToolSelectionSettings() const { return tool_selection_.Save(); }
  void RestoreToolSelection(const json& settings);
  json PreviewContext();
  void PublishMessage(const std::string& request_id = "");
  const json& Statistics() const { return conversation_.Statistics(); }
  void Rename(std::string title) {
    session_title_ = std::move(title);
    custom_title_ = true;
    ++revision_;
  }
  // Name new sessions with UAGENT_TITLE_MODEL in the background. Only
  // persistent interactive hosts enable it; one-shot runs keep the first line.
  void GenerateTitles(bool enabled) { generate_titles_ = enabled; }

  json ModelRequest();

  bool Save(const std::string& path, std::string& error) const;

  bool Load(const std::string& path, const std::string& expected_cwd,
            std::string& error);

  // Estimated tokens in the request currently represented by the conversation.
  // Provider usage belongs to billing and may be cumulative or stale.
  int64_t ContextUsed() const;

  // summarize the conversation with the model, then restart the session
  // from that summary — frees the context without losing the thread
  bool Compact(bool automatic = false, Usage* turn_usage = nullptr);

  // Fold in what subagent processes spent. They bill against the same key, so
  // without this their cost is missing from the footer and the status bar.
  void DrainSubagentUsage();

  // Account finished side work. What the current turn spawned merges into
  // `turn` when given; other tagged delegated work updates its originating
  // turn; untagged side work remains visible in the session total without
  // being assigned to the wrong turn.
  void AccountSideUsage(Usage* turn = nullptr);

  void MergeSessionUsage(const Usage& usage);

  // Report finished background jobs to the user and hand them to the model.
  // The drain reaps and deletes each log, so its caller owns the only copy.
  // `children_finished` says whether a delegated child was among them.
  bool DrainBackground(bool* children_finished = nullptr);
  void ReportMemoryCompletion(BackgroundCompletion& completion);
  void DeliverActivityCompletions(
      const std::vector<BackgroundCompletion>& completions);

  // Takes this session's pending mail (core/mailbox.h) into the conversation:
  // a wake or step message as queued guidance, which the host starts a turn
  // with when the message wakes; a passive one as a harness note; an
  // interrupt stops the running turn after queueing its text. With `hold`,
  // wake messages stay pending. True when anything was taken.
  bool DeliverMail(bool hold = false);
  // Text about to arrive as input that another session or the harness wrote:
  // its row is shown as an event, not as the person's message, under its
  // `author` when it has one.
  void NotFromUser(const std::string& text, const std::string& author = "") {
    not_user_.emplace_back(text, author);
  }
  // A coordinator's chat (app/chat.h): `said` hears what a person writes
  // here, `heard` is handed each message from another session before it is
  // delivered and may rewrite it.
  void SetChat(std::function<void(const std::string&)> said,
               std::function<void(Mail&)> heard) {
    chat_said_ = std::move(said);
    chat_heard_ = std::move(heard);
  }

  // The session's undo (EditJournal), kept in `directory`; unset, nothing is
  // journaled, as for a subagent.
  void OpenEditJournal(std::string directory) {
    edits_.Open(std::move(directory));
  }
  // The files `turn` changed (0: the latest turn that changed any).
  json ChangedFiles(int64_t turn) {
    return edits_.Files(turn ? turn : edits_.LastTurn());
  }
  // Puts them back (all, or only `path`); the model hears which at its next
  // step, so it re-reads before editing them again.
  json Revert(int64_t turn, const std::string& path);

  // Files the model attached ride in on a user message. Canonical tool results
  // are text-only, so image/file parts cannot travel with them.
  bool DrainAttachments();
  // One attachment message for `attachments`, or a note saying why they
  // could not be attached; true when they were.
  bool PushAttachments(const std::vector<Attachment>& attachments);
  // Starts the side call that names this session; DrainBackground applies
  // its answer unless the user renamed the session meanwhile.
  void StartTitle(const std::string& user_input);

  // one user turn: stream, run tools, repeat until prose; prints as it goes
  void Turn(const std::string& user_input, json user_content = nullptr,
            const json& images = json::array(),
            const std::string& request_id = "");

 private:
  struct TurnExecution;
  struct StepState;

  struct ActivityPollResult {
    int64_t id = 0;
    bool ok = false;
    bool no_change = false;
    bool terminal = false;
  };

  struct ToolRejection {
    std::string tool;
    std::string issue_code;
    std::string issue_field;
    std::string operation;
  };

  // What the step loop does next. kRetryStep repeats the step without
  // spending one of the turn's budget.
  enum class StepFlow {
    kProceed,
    kRetryStep,
    kNextStep,
    kEndTurn,
  };

  std::string AnalyzeImageContent(const json& content, std::string& error);
  std::string ApplyImageAnalysisFallback(json& messages, bool analyze = true,
                                         json* deliveries = nullptr);

  Usage AccountModelUsage(const json& reported);

  void FailTurn(TurnExecution& state, std::string message);
  void FailBudget(TurnExecution& state, TurnStopReason reason,
                  std::string message);
  bool TurnDeadlineExceeded(TurnExecution& state,
                            std::chrono::seconds reserve = {});
  bool TurnCostExceeded(TurnExecution& state);
  bool TurnTokenBudgetExceeded(TurnExecution& state, bool before_model = false);
  void RecordModelResponse(
      ChatResult& response, TurnExecution& state,
      std::unordered_map<std::string, int64_t>& tool_counts);
  bool ToolCallsWithinLimits(const std::vector<ToolCall>& calls,
                             TurnExecution& state, StepState& loop);
  void AnswerAtLimit(TurnExecution& state, StepState& loop);
  void FinishTurn(TurnExecution& state, int64_t step);
  void UpdateTurnSideUsage(int64_t turn, const Usage& usage,
                           const json& statistics);

  // One step of the turn, in the order the loop runs them. Each phase reports
  // what the loop should do next.
  void PushSkillContext(std::string skill);
  void PushUserInput(json content, bool attachment, const json& images,
                     const std::string& request_id);
  void PushStepNote(StepState& loop, const std::string& note);
  StepFlow InterruptTurn(TurnExecution& state);
  bool ApplyQueuedSteering(StepState& loop);
  StepFlow PrepareStep(TurnExecution& state, StepState& loop);
  StepFlow HandleFailedResponse(ChatResult& response, TurnExecution& state,
                                StepState& loop, const json& schemas,
                                bool attachment);
  // Counts one more of a model's faults; false once that ends the turn.
  // `advise` tells the model what its rule says at this strike.
  bool Strike(Fault fault, TurnExecution& state, StepState& loop,
              const std::string& about = "", bool advise = true);
  void Advise(Fault fault, StepState& loop, const std::string& about = "");
  StepFlow HandleResponseStop(ChatResult& response, size_t tool_call_count,
                              TurnExecution& state, StepState& loop);
  void RecordToolRoundRepetition(const std::vector<ToolCall>& calls,
                                 StepState& loop);
  bool HandleActivityPollResults(const std::vector<ActivityPollResult>& polls,
                                 bool exclusive, TurnExecution& state,
                                 StepState& loop);
  bool StopForRepeatedRejections(const std::vector<ToolRejection>& rejections,
                                 TurnExecution& state, StepState& loop);
  void PushAssistantMessage(ChatResult& response,
                            const std::vector<ToolCall>& calls);
  StepFlow FinishWithProse(TurnExecution& state, StepState& loop);
  StepFlow ExecuteToolCalls(const std::vector<ToolCall>& calls,
                            TurnExecution& state, StepState& loop);

  ChatResult Chat(const char* purpose, int64_t step, const json& schemas,
                  const json* request_messages = nullptr);
  void AnnounceDeliveries(const json& deliveries);
  void DebugModelRequest(int64_t request, int64_t step, const char* purpose,
                         const json& schemas, size_t schema_bytes,
                         const json& messages, size_t message_bytes,
                         const json* request_messages, bool projected);
  void DebugModelResponse(int64_t request, int64_t step, const char* purpose,
                          const ChatResult& result);
  // Leaves projected null when the source already needs no preparation.
  std::string PrepareRequestMessages(const json& source, json& projected,
                                     bool analyze, json* deliveries = nullptr);
  json CompactionMessages() const;
  // Skill instructions this conversation loaded, newest first within a
  // bound: a summary cannot stand in for a procedure being followed.
  json CompactionSkillMessages() const;
  // Retained recent user instructions for the post-compaction context.
  // Optionally fills retained_ids with the source display id per message
  // (same order), so the re-push can keep its identity instead of minting
  // a duplicate block beside the archived original.
  json CompactionUserMessages(
      std::vector<uint64_t>* retained_ids = nullptr) const;

  size_t RequestContextBytes(size_t schema_bytes,
                             const json* messages = nullptr) const;
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

  MidturnCompact MaybeCompactDuringTurn(Usage& usage, size_t& turn_start);

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
  std::string PromptBase() const;
  json PromptContext() const;
  void RefreshSystemMessage(bool force = false);
  std::string RuntimeContextText() const;

  // Message 0 is the one place the system shape is defined. Always rebuilt
  // rather than restored, so it tracks the current tools/protocol (see load()).
  json SysMsg() const;
  void EnsureRuntimeContext();
  json CoordinatorRequest(json messages) const;

  // Append environment state only when it changes. This preserves every prior
  // request byte for provider caching without repeating cwd metadata each turn.

  std::string ProjectInstructionText() const;
  std::string MemoryText() const;

  size_t BaselineSize() const;

  json BaselineMessages() const;

  std::vector<MessageKind> BaselineKinds() const;

  void RefreshBaseline();

  // Keep receipts and read metadata from the original; send only `result`.
  void AppendToolResult(const ToolCall& call, const std::string& result,
                        const ToolResult& original, double duration_ms);

  // Pushes a tool_result message together with the ids the retained view
  // and the UI join on. No push path may skip the metadata half.
  void PushToolResultMessage(const ToolCall& call, json message);

  // returns true if the user interrupted the batch
  void PrepareCall(const ToolCall& call, CallTask& task, TurnExecution& state,
                   StepState& loop);
  bool RunCalls(const std::vector<ToolCall>& calls, TurnExecution& state,
                StepState& loop, std::vector<ToolRejection>& rejections,
                std::vector<ActivityPollResult>& activity_polls);

  void RebuildToolSchemas();
  // After the tool set or its selection changed: the offered schemas, their
  // size and the logged copy are stale.
  void InvalidateToolSchemas(bool force_system);
  void SyncApiSessionUsage();
  std::vector<std::string> ExplicitSkillContext(
      const std::string& user_input) const;

  Api& api_;
  std::vector<Tool>& tools_;
  ProcessSupervisor& processes_;
  UsageAccumulator& side_usage_;
  json schemas_;  // request-shaped tool schemas, rebuilt after MCP changes
  ToolSelection tool_selection_;
  ToolSchemaCache available_schemas_;
  size_t schema_bytes_ = 0;
  Approver approve_;
  ToolRefresher refresh_tools_;
  ProjectInstructions project_instructions_;
  std::vector<Skill> skills_;
  AdaptiveSystemState* adaptive_system_ = nullptr;
  mutable std::string prompt_error_;
  std::string last_sent_prompt_;
  Conversation conversation_;
  SearchTrace turn_search_trace_;
  Usage session_usage_;
  RouteUsage route_usage_;
  std::string session_id_;
  bool retain_exchanges_ = false;
  std::string turn_root_, reply_to_, reply_excerpt_;
  json image_analyses_ = json::object();
  mutable FileLease writer_;
  std::string session_title_;
  bool custom_title_ = false;
  // Fork lineage, restored on load and preserved on save so branches keep
  // their parent link across worker generations.
  std::string parent_session_id_;
  int64_t forked_at_turn_ = 0;
  std::string forked_at_time_;
  json session_role_ = json::object();
  // Mail taken but not yet in a saved snapshot, acknowledged by Save, and the
  // ids of the latest delivered, so one delivered again is recognised.
  mutable std::vector<std::string> unacked_mail_;
  std::vector<std::pair<std::string, std::string>> not_user_;  // text, author
  std::function<void(const std::string&)> chat_said_;
  std::function<void(Mail&)> chat_heard_;
  json delivered_mail_ = json::array();
  EditJournal edits_;
  uint64_t view_epoch_ = 0;
  std::function<std::string()> runtime_context_;
  int64_t total_user_turns_ = 0;
  size_t logged_msgs_ = 0;      // messages already written to the debug trace
  std::string logged_schemas_;  // last exact per-request schema snapshot
  int64_t turn_id_ = 0;
  int64_t request_id_ = 0;
  uint64_t revision_ = 0;
  bool cost_warning_shown_ = false;
  bool token_warning_shown_ = false;
  std::chrono::steady_clock::time_point active_deadline_ =
      std::chrono::steady_clock::time_point::max();
  std::string last_error_;
  // The attachment warning last shown, so a turn says it once, not per step.
  std::string attachment_warning_;
  json last_stop_;
  json turn_side_statistics_ = json::object();
  KeepFile keep_tool_file_;
  // What a side question needs, captured on the turn thread so the side
  // thread never reads the live route or conversation.
  struct SideContext {
    std::shared_ptr<const json> messages, tools;
    ApiSettings api;
    std::string session_id;
  };
  mutable std::mutex side_mutex_;
  std::shared_ptr<const SideContext> side_context_;
  bool generate_titles_ = false;
  std::mutex title_mutex_;
  std::string generated_title_;
  // Last member: destroying or replacing it stops and joins the call before
  // the fields it writes go away.
  std::jthread title_thread_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_H_
