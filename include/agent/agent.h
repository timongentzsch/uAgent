// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_AGENT_H_
#define UAGENT_INCLUDE_AGENT_AGENT_H_
// The agent loop: an Agent owns its message history and drives
// model -> tool -> model until the model answers in prose.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/agent/conversation.h"
#include "include/agent/edit_journal.h"
#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/agent/trace.h"
#include "include/api/client.h"
#include "include/core/json.h"
#include "include/core/lease.h"
#include "include/core/project.h"
#include "include/core/skills.h"
#include "include/core/thread_annotations.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"

namespace uagent {
struct Mail;

struct ActivityCompletion;
struct CallTask;
enum class TurnStopReason;
enum class Fault;

class Agent {
 public:
  // Returns why a mutating call was refused, empty when it may run.
  using Approver =
      std::function<std::string(const Tool&, const json& args, int64_t turn)>;
  using ToolRefresher =
      std::function<bool(std::chrono::steady_clock::time_point)>;

  Agent(Api& api, std::vector<Tool>& tools, ActivitySupervisor& processes,
        UsageAccumulator& side_usage, Approver approve,
        ToolRefresher refresh_tools = {},
        ProjectInstructions project_instructions = {},
        std::vector<Skill> skills = {});

  void Reset();
  // What the model reads now, with its sources.
  json PromptPreview() const;
  const std::string& LastSentPrompt() const { return last_sent_prompt_; }

  const Usage& SessionUsage() const { return session_usage_; }
  json RouteUsageJson() const { return uagent::RouteUsageJson(route_usage_); }
  const std::string& LastError() const { return last_error_; }
  // Why the last turn ended, with the limits in force; empty before the first.
  const json& LastStop() const { return last_stop_; }
  const std::string& SessionId() const { return session_id_; }
  uint64_t Revision() const { return revision_; }
  // The most recent completed turn's archived tool traffic, as data.
  json LatestToolTrace() const;

  // The final assistant prose: the whole result of a headless (-p) run.
  std::string LastText() const { return conversation_.LastAssistantText(); }

  size_t MessageCount() const { return conversation_.Size(); }

  void RouteChanged();

  std::string ActiveRoute() const;

  // The vision route for image analysis; empty means the main route, or none.
  std::string EffectiveImageModel() const;

  // Session picker's one-line title.
  std::string FirstUserText() const;

  // Real user prompts are tracked out of band from model-readable text.
  int64_t UserTurns() const;

  const Conversation& History() const { return conversation_; }
  json DisplaySnapshot() const;
  // Forks a resumable side conversation (/btw); safe beside a running turn.
  json SideConversation(const std::string& source,
                        const std::string& question) const;
  void PublishSideContext();
  // Snapshots a tool's file for clients: {id, name, mime, bytes} or null.
  using KeepFile =
      std::function<json(const std::string& path, const std::string& name)>;
  void KeepToolFiles(KeepFile keep) { keep_tool_file_ = std::move(keep); }
  // An agent's role ({agent}), saved in the session header.
  void SetSessionRole(json role) { session_role_ = std::move(role); }
  // Bills every response under this purpose instead of the conversation.
  void SetUsagePurpose(std::string purpose) {
    usage_purpose_ = std::move(purpose);
  }
  void RetainExchanges(bool enabled) {
    retain_exchanges_ = enabled;
    api_.capture_http = enabled;
  }
  json HttpExchanges() const;
  json SessionSettings() const;
  void SessionSettings(const json& settings);
  // The items its last todo call left; empty when it has none.
  json Todo() const;
  json ToolCatalogue() const;
  // The tools switched on, by name: the most a delegated child may be given.
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
  // Names new sessions with UAGENT_TITLE_MODEL in the background.
  void GenerateTitles(bool enabled) { generate_titles_ = enabled; }

  json ModelRequest();

  bool Save(const std::string& path, std::string& error) const;

  bool Load(const std::string& path, const std::string& expected_cwd,
            std::string& error);

  // An estimate: provider usage may be cumulative or stale.
  int64_t ContextUsed() const;

  // Summarizes the conversation with the model and restarts from the summary.
  bool Compact(bool automatic = false, Usage* turn_usage = nullptr);

  // Folds in what this session's children reported to its ledger.
  void DrainChildUsage();

  // What this turn spawned merges into `turn`; other work goes to its own turn.
  void AccountSideUsage(Usage* turn = nullptr);

  void MergeSessionUsage(const Usage& usage);

  // Hands finished background jobs to the model, as a turn with `start_turn`.
  bool DrainActivities(bool start_turn = false);
  void ReportMemoryCompletion(ActivityCompletion& completion);
  void DeliverActivityCompletions(
      const std::vector<ActivityCompletion>& completions);
  // Gives a backgrounded command's row its result; false when no row waits.
  bool FinishCommandRow(const EndedCommand& ended);

  // Takes the pending mail session_links.h allows; true when any was taken.
  bool DeliverMail();
  // Starts a turn when idle; a working session reads it at its next step.
  void Arrive(const std::string& text, json origin);
  // Writes a message into the conversation without a turn.
  void Say(const std::string& text, const std::string& request_id,
           const json& origin = nullptr) {
    PushUserInput(text, false, json(), request_id, origin);
  }

  // Unset, nothing is journaled.
  void OpenEditJournal(std::string directory) {
    edits_.Open(std::move(directory));
  }
  // The files `turn` changed (0: the latest turn that changed any).
  json ChangedFiles(int64_t turn) {
    return edits_.Files(turn ? turn : edits_.LastTurn());
  }
  // Puts them back; the model hears which at its next step, to re-read them.
  json Revert(int64_t turn, const std::string& path);

  // Tool results are text-only: attached files ride in on a user message.
  bool DrainAttachments();
  // False when they could not be attached; a note then says why.
  bool PushAttachments(const std::vector<Attachment>& attachments);
  // DrainActivities applies the title unless the user renamed the session.
  void StartTitle(const std::string& user_input);

  // Runs until prose; `origin` (Steering::Message) names a non-person writer.
  void Turn(const std::string& user_input, json user_content = nullptr,
            const json& images = json::array(),
            const std::string& request_id = "", const json& origin = nullptr);
  // A call-shaped row in the last turn for what the host did as it ended.
  void RecordReceipt(const Headline& headline, json parts);

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
    std::string message;  // what the model was told
  };

  // kRetryStep repeats the step without spending one of the turn's budget.
  enum class StepFlow {
    kProceed,
    kRetryStep,
    kNextStep,
    kEndTurn,
  };

  const RouteResolution& ImageRoute() const;
  std::string AnalyzeImageContent(const json& content, std::string& error);
  std::string ApplyImageAnalysisFallback(json& messages, bool analyze = true,
                                         json* deliveries = nullptr);

  Usage AccountModelUsage(const json& reported,
                          const std::string& purpose = {});

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

  void PushSkillContext(std::string skill);
  void PushUserInput(json content, bool attachment, const json& images,
                     const std::string& request_id,
                     const json& origin = nullptr);
  void PushStepNote(StepState& loop, const std::string& note);
  StepFlow InterruptTurn(TurnExecution& state);
  bool ApplyQueuedSteering(StepState& loop);
  // Counts the turns mail starts in a row (agent.streak); a person resets it.
  void CountStreak(const json& origin);
  StepFlow PrepareStep(TurnExecution& state, StepState& loop);
  StepFlow HandleFailedResponse(ChatResult& response, TurnExecution& state,
                                StepState& loop, const json& schemas,
                                bool attachment);
  // Counts one more of a model's faults; false once that ends the turn.
  bool Strike(Fault fault, TurnExecution& state, StepState& loop,
              const std::string& about = "", bool advise = true);
  void Advise(Fault fault, StepState& loop, const std::string& about = "");
  StepFlow HandleResponseStop(ChatResult& response, size_t tool_call_count,
                              TurnExecution& state, StepState& loop);
  bool HandleActivityPollResults(const std::vector<ActivityPollResult>& polls,
                                 bool exclusive, TurnExecution& state,
                                 StepState& loop);
  bool StopForRepeatedRejections(const std::vector<ToolRejection>& rejections,
                                 TurnExecution& state, StepState& loop);
  void PushAssistantMessage(ChatResult& response,
                            const std::vector<ToolCall>& calls);
  StepFlow FinishWithProse(TurnExecution& state, StepState& loop);
  StepFlow ExecuteToolCalls(const std::vector<ToolCall>& calls,
                            const std::string& served_by, TurnExecution& state,
                            StepState& loop);

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
  // Newest first within a bound: a summary cannot replace a procedure in use.
  json CompactionSkillMessages() const;
  // `retained_ids` receives each message's display id, so a re-push keeps it.
  json CompactionUserMessages(
      std::vector<uint64_t>* retained_ids = nullptr) const;

  size_t RequestContextBytes(size_t schema_bytes,
                             const json* messages = nullptr) const;
  // The byte estimate, corrected by what the provider last reported beyond it.
  int64_t ContextTokens(size_t bytes) const;
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

  // Encoded attachment bytes are never durable, the model's mid-turn ones too.
  void PruneAttachments(size_t turn_start);

  // Completed tool messages stay in context and are also archived for /trace.
  void ArchiveTurnTrace(size_t turn_start);
  void PruneOldToolResults();

  // Drops a rejected capability and retries. Most specific first: the
  // native-tools probe matches any "tool" and must stay last.
  bool DegradeAndRetry(const ChatResult& result);

  std::string SystemPrompt() const;
  std::string PromptBase() const;
  json PromptContext() const;
  void RefreshSystemMessage(bool force = false);
  std::string RuntimeContextText() const;

  // Message 0: rebuilt, never restored, so it tracks the current tools.
  json SysMsg() const;
  // Appends environment state only when it changes, keeping the cached prefix.
  void EnsureRuntimeContext();
  // What a request carries beyond the stored conversation: the to-do list.
  std::string RequestContext() const;
  json WithRequestContext(json messages, std::string context) const;

  std::string ProjectInstructionText() const;
  std::string MemoryText() const;

  size_t BaselineSize() const;

  json BaselineMessages() const;

  std::vector<MessageKind> BaselineKinds() const;

  void RefreshBaseline();

  // Keep receipts and read metadata from the original; send only `result`.
  void AppendToolResult(const ToolCall& call, const std::string& result,
                        const ToolResult& original, double duration_ms);

  // Pushes the result with the ids the retained view and the UI join on.
  void PushToolResultMessage(const ToolCall& call, json message);

  void PrepareCall(const ToolCall& call, CallTask& task, TurnExecution& state,
                   StepState& loop);
  void PublishWork(const std::vector<CallTask>& tasks);
  // Ends the work replying to the latest message of yours as `outcome`.
  void CloseWork(const std::string& outcome);
  // True when the user interrupted the batch.
  bool RunCalls(const std::vector<ToolCall>& calls, TurnExecution& state,
                StepState& loop, std::vector<ToolRejection>& rejections,
                std::vector<ActivityPollResult>& activity_polls);

  void RebuildToolSchemas();
  void InvalidateToolSchemas(bool force_system);
  void SyncApiSessionUsage();
  std::vector<std::string> ExplicitSkillContext(const std::string& user_input);
  // Offers the tools an opened skill unlocks (Tool::skill) from then on.
  void UnlockSkillTools(const std::string& skill);

  Api& api_;
  std::vector<Tool>& tools_;
  ActivitySupervisor& processes_;
  UsageAccumulator& side_usage_;
  json schemas_;  // request-shaped tool schemas, rebuilt after MCP changes
  ToolSelection tool_selection_;
  ToolSchemaCache available_schemas_;
  size_t schema_bytes_ = 0;
  Approver approve_;
  ToolRefresher refresh_tools_;
  ProjectInstructions project_instructions_;
  std::vector<Skill> skills_;
  // Read once: a later edit must not rewrite a running conversation's prefix.
  std::string prompt_overlay_digest_;
  json prompt_overlay_;
  mutable std::string prompt_error_;
  std::string last_sent_prompt_;
  Conversation conversation_;
  Usage session_usage_;
  RouteUsage route_usage_;
  std::string session_id_;
  bool retain_exchanges_ = false;
  // The turn's message, and the latest message of yours the work replies to.
  std::string turn_root_, reply_to_;
  // What that work's calls did and when it began, for its work node.
  struct Work {
    int64_t steps = 0, errors = 0;
    std::set<std::string> edited;
    std::chrono::steady_clock::time_point started;
  } work_;
  // Image analyses by key, the most recently used first.
  std::list<std::pair<std::string, std::string>> image_analyses_;
  // Images whose analysis failed this turn, so later steps do not ask again.
  std::set<std::string> failed_image_analyses_;
  mutable std::pair<std::string, RouteResolution> image_route_;
  mutable FileLease writer_;
  mutable SavedFile saved_file_;
  std::string session_title_;
  bool custom_title_ = false;
  // Fork lineage, kept across load and save.
  std::string parent_session_id_;
  int64_t forked_at_turn_ = 0;
  std::string forked_at_time_;
  json session_role_ = json::object();
  // Mail taken but not yet in a saved snapshot; Save acknowledges it.
  mutable std::vector<std::string> unacked_mail_;
  // Mail not in the conversation yet: a restart receives it again.
  std::vector<std::string> arriving_mail_;
  // The ids of the latest delivered, so one delivered again is recognised.
  json delivered_mail_ = json::array();
  EditJournal edits_;
  int64_t total_user_turns_ = 0;
  size_t logged_msgs_ = 0;      // messages already written to the debug trace
  std::string logged_schemas_;  // last exact per-request schema snapshot
  int64_t turn_id_ = 0;
  int turn_hops_ = 0;  // what mail sent in this turn carries (Arrive)
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
  std::string usage_purpose_;
  // What the provider's reported prompt held beyond the byte estimate.
  int64_t context_offset_ = 0;
  KeepFile keep_tool_file_;
  // Captured on the turn thread: the side thread never reads live state.
  SessionRecord Snapshot() const;
  mutable std::mutex side_mutex_;
  struct SideCheckpoint {
    SessionRecord record;
    std::shared_ptr<const json> archive;
  };
  std::shared_ptr<const SideCheckpoint> side_context_
      UAGENT_GUARDED_BY(side_mutex_);
  bool generate_titles_ = false;
  std::mutex title_mutex_;
  std::string generated_title_ UAGENT_GUARDED_BY(title_mutex_);
  // Last member: its destructor joins before the fields it writes go away.
  std::jthread title_thread_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_AGENT_H_
