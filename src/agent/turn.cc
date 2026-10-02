// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/agent/protocol.h"
#include "include/agent/trace.h"
#include "include/api/citations.h"
#include "include/api/retry.h"
#include "include/core/checked.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/skills.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "src/agent/turn_internal.h"

namespace uagent {
namespace {
bool GenericSessionTitle(std::string title) {
  title = AsciiLower(Trim(title));
  return title == "hi" || title == "hello" || title == "hey" || title == "test";
}

}  // namespace
std::vector<std::string> Agent::ExplicitSkillContext(
    const std::string& user_input) const {
  std::vector<std::string> selected;
  for (const Skill& skill : skills_) {
    std::string mention = "$" + skill.name;
    size_t at = 0;
    bool named = false;
    while ((at = user_input.find(mention, at)) != std::string::npos) {
      size_t end = at + mention.size();
      if (end == user_input.size() ||
          (!isalnum(static_cast<unsigned char>(user_input[end])) &&
           user_input[end] != '-' && user_input[end] != '_')) {
        named = true;
        break;
      }
      at = end;
    }
    if (!named) continue;
    SkillReadResult result = ReadSkillBody(skill);
    if (!result.ok) {
      Emit(NoticeEvent(
          PresentationStatus::kWarned,
          "skill " + skill.name + " unavailable: " + result.output));
      continue;
    }
    Emit(
        NoticeEvent(PresentationStatus::kNeutral, "using skill " + skill.name));
    selected.push_back(std::move(result.output));
  }
  return selected;
}

void Agent::PushSkillContext(std::string skill) {
  conversation_.Push(
      HarnessMessage("[explicit skill instructions; user selected]\n" +
                     std::move(skill)),
      MessageKind::kInternal);
}

// The person's message, with the files the transcript shows for it.
void Agent::PushUserInput(json content, bool attachment, const json& images,
                          const std::string& request_id) {
  conversation_.Push(
      {{"role", "user"}, {"content", std::move(content)}},
      attachment ? MessageKind::kAttachment : MessageKind::kUser);
  if (images.is_array() && !images.empty()) {
    conversation_.RecordDisplay(conversation_.LastDisplayId(),
                                {{"files", images}});
  }
  if (const auto mail = std::ranges::find(
          not_user_, conversation_.LastText(MessageKind::kUser));
      mail != not_user_.end()) {
    not_user_.erase(mail);
    conversation_.RecordDisplay(conversation_.LastDisplayId(),
                                {{"origin", "mail"}});
  }
  PublishMessage(request_id);
}

// A harness note for this step only; several in one step go out as one.
void Agent::PushStepNote(StepState& loop, const std::string& note) {
  if (!loop.note.empty()) loop.note += "\n\n";
  loop.note += note;
}

// Every interruption ends the turn the same way, whatever noticed it first.
Agent::StepFlow Agent::InterruptTurn(TurnExecution& state) {
  state.stop.outcome = TurnOutcome::kInterrupted;
  last_error_ = TurnOutcomeName(state.stop.outcome);
  return StepFlow::kEndTurn;
}

// Steering joins the conversation as ordinary user messages, and every
// per-step recovery counter starts over: the question has changed.
bool Agent::ApplyQueuedSteering(StepState& loop) {
  std::vector<Steering::Message> queued = SteeringState().TakeMessages();
  if (queued.empty()) return false;
  SteeringState().Take();
  for (auto& message : queued) {
    std::string& input = message.text;
    for (std::string& skill : ExplicitSkillContext(input)) {
      PushSkillContext(std::move(skill));
    }
    // Steered files join the steered message exactly like submitted ones:
    // composed content for the model, display files for the transcript.
    // A compose failure degrades to text (the claim validated the files
    // seconds ago) rather than failing the running turn.
    if (message.attachments.is_array() && !message.attachments.empty()) {
      std::string error;
      auto [content, attachment] =
          ComposeSteeredContent(input, message.attachments, error);
      if (error.empty()) {
        PushUserInput(std::move(content), attachment,
                      attachment ? message.images : json(), message.request_id);
        continue;
      }
      DebugLog("steering_attachments_failed",
               {{"turn", turn_id_}, {"error", error}});
    }
    PushUserInput(std::move(input), false, json(), message.request_id);
  }
  loop.recovery = {};
  DebugLog("steering_applied",
           {{"turn", turn_id_}, {"messages", queued.size()}});
  return true;
}

// Everything that happens before the model call: steering, a refreshed system
// message, the budget gates, and the schemas this step is allowed to offer.
Agent::StepFlow Agent::PrepareStep(TurnExecution& state, StepState& loop) {
  // Mail from its parent, children, coordinator or linked sessions arrives as
  // steering and is applied by the very next statement.
  DeliverMail();
  ApplyQueuedSteering(loop);
  RefreshSystemMessage();
  if (!prompt_error_.empty()) {
    FailTurn(state, prompt_error_);
    return StepFlow::kEndTurn;
  }
  if (SteeringState().Requested()) return InterruptTurn(state);
  if (TurnDeadlineExceeded(state)) return StepFlow::kEndTurn;
  if (refresh_tools_ && refresh_tools_(state.deadline)) RebuildToolSchemas();
  if (TurnDeadlineExceeded(state)) return StepFlow::kEndTurn;
  DrainBackground();
  AccountSideUsage(&state.metrics.usage);
  if (TurnTokenBudgetExceeded(state, /*before_model=*/true)) {
    return StepFlow::kEndTurn;
  }
  if (TurnCostExceeded(state)) return StepFlow::kEndTurn;
  ToolAvailability availability{
      .detached_terminal = processes_.PendingCount() > 0 ||
                           processes_.DetachedCount() > 0 ||
                           loop.detached_records_available,
  };
  available_schemas_.Get(tools_, schemas_, loop.tool_counts, availability,
                         &tool_selection_);
  if (loop.step > 0 && loop.midturn_compaction_enabled) {
    MidturnCompact compacted =
        MaybeCompactDuringTurn(state.metrics.usage, state.start);
    if (compacted != MidturnCompact::kNotNeeded) {
      loop.midturn_compaction_enabled = false;
      return StepFlow::kRetryStep;
    }
  }
  return StepFlow::kProceed;
}

// An interrupted or failed model call; kProceed means the response is usable.
bool Agent::HandleActivityPollResults(
    const std::vector<ActivityPollResult>& polls, bool exclusive,
    TurnExecution& state, StepState& loop) {
  int64_t& quiet_polls =
      loop.recovery.strikes[static_cast<size_t>(Fault::kQuietPoll)];
  bool successful = false;
  for (const ActivityPollResult& poll : polls) successful |= poll.ok;
  if (successful) {
    loop.recovery.last_call.clear();
    loop.recovery.strikes[static_cast<size_t>(Fault::kRepeat)] = 0;
  }
  // Only a lone poll that came back running and unchanged counts.
  if (polls.size() != 1 || !exclusive || !polls.front().ok ||
      polls.front().terminal || !polls.front().no_change) {
    if (!polls.empty()) quiet_polls = 0;
    return false;
  }
  const int64_t id = polls.front().id;
  if (loop.recovery.quiet_activity_id != id) quiet_polls = 0;
  loop.recovery.quiet_activity_id = id;
  return !Strike(Fault::kQuietPoll, state, loop, std::to_string(id));
}

Agent::StepFlow Agent::ExecuteToolCalls(const std::vector<ToolCall>& calls,
                                        TurnExecution& state, StepState& loop) {
  if (state.line_open) printf("\n");
  std::vector<ToolRejection> rejections;
  std::vector<ActivityPollResult> activity_polls;
  bool cancelled = RunCalls(calls, state, loop, rejections, activity_polls);
  state.line_open = false;
  bool foreground_interrupted = SteeringState().Requested() || cancelled;
  bool steering_applied = ApplyQueuedSteering(loop);
  if (cancelled) BgCancelSubagents(processes_);
  if (foreground_interrupted) {
    if (steering_applied) return StepFlow::kNextStep;
    return InterruptTurn(state);
  }
  if (HandleActivityPollResults(activity_polls, calls.size() == 1, state,
                                loop)) {
    return StepFlow::kEndTurn;
  }
  if (StopForRepeatedRejections(rejections, state, loop)) {
    return StepFlow::kEndTurn;
  }
  if (rejections.empty() &&
      loop.recovery.strikes[static_cast<size_t>(Fault::kFailedTools)] == 0) {
    Advise(Fault::kRepeat, loop);
  }
  // Do not start a network request with only curl's one-second granularity
  // left after tools. Report the owning turn budget instead of a misleading
  // transport timeout that cannot possibly be retried.
  if (TurnDeadlineExceeded(
          state, std::chrono::seconds(kModelRequestDeadlineReserveSeconds))) {
    return StepFlow::kEndTurn;
  }
  return StepFlow::kNextStep;
}

void Agent::Turn(const std::string& user_input, json user_content,
                 const json& images, const std::string& request_id) {
  last_error_.clear();
  // A new turn owns its outcome: clients must never re-report the
  // previous turn's stop from a boundary publish before this one ends.
  last_stop_ = nullptr;
  ++turn_id_;
  turn_side_statistics_ = json::object();
  turn_root_.clear();
  reply_to_.clear();
  reply_excerpt_.clear();
  ++revision_;
  ++total_user_turns_;
  std::string title = FirstLine(user_input);
  if (session_title_.empty() ||
      (!custom_title_ && GenericSessionTitle(session_title_) &&
       title.size() >= kGenericTitleReplacementMinChars &&
       !GenericSessionTitle(title))) {
    if (generate_titles_ && !GenericSessionTitle(title)) StartTitle(user_input);
    session_title_ = std::move(title);
  }
  std::string local_time = LocalStamp();
  RefreshSystemMessage();
  Emit(Event{EventId::kTurnStarted,
             {{"turn", turn_id_},
              {"origin", "user"},
              {"local_time", local_time},
              {"input", user_input},
              {"attachments", user_content.is_array() && !user_content.empty()
                                  ? user_content.size() - 1
                                  : 0},
              {"messages", conversation_.Size()},
              {"context_tokens", ContextUsed()}}});
  bool attachment = !user_content.is_null();
  std::vector<std::string> explicit_skills = ExplicitSkillContext(user_input);
  size_t skill_bytes = 0;
  for (const std::string& skill : explicit_skills) {
    skill_bytes = SaturatingAdd(skill_bytes, skill.size());
  }
  size_t pending_bytes = SaturatingAdd(
      attachment ? JsonEstimatedBytes(user_content) : user_input.size(),
      skill_bytes);
  int64_t pressure = 0;
  int64_t projected_tokens = 0;
  if (ContextNeedsCompaction(pending_bytes, schema_bytes_, pressure,
                             projected_tokens)) {
    DebugLog("auto_compact", {{"turn", turn_id_},
                              {"projected_pct", pressure},
                              {"projected_tokens", projected_tokens}});
    Compact(true);
  }
  if (SteeringState().Requested() && SteeringState().SteerCount() == 0) {
    PushUserInput(attachment ? std::move(user_content) : json(user_input),
                  attachment, images, request_id);
    Emit(NoticeEvent(PresentationStatus::kWarned, "interrupted"));
    Emit(Event{EventId::kTurnStopped,
               {{"turn", turn_id_}, {"outcome", "interrupted"}, {"steps", 0}}});
    return;
  }
  EnsureRuntimeContext();
  TurnExecution state;
  state.model_calls_before =
      JsonValue(conversation_.Statistics(), "model_calls", int64_t{0});
  StepState loop;
  state.start = conversation_.Size();  // user message and prune_* start
  for (std::string& skill : explicit_skills) PushSkillContext(std::move(skill));
  PushUserInput(attachment ? std::move(user_content) : json(user_input),
                attachment, images, request_id);
  turn_search_trace_.Reset();
  // Slices the budget block out of the config: a turn-boundary reload may
  // replace api_.config mid-session, and the limits this turn is judged
  // against are the ones it started with.
  state.limits = api_.config;
  state.deadline =
      state.limits.max_turn_seconds > 0
          ? DeadlineAfter(state.started, state.limits.max_turn_seconds)
          : std::chrono::steady_clock::time_point::max();
  active_deadline_ = state.deadline;
  loop.detached_records_available = !DetachedRecords().empty();

  for (; state.limits.max_steps <= 0 || loop.step < state.limits.max_steps;
       ++loop.step) {
    StepFlow flow = PrepareStep(state, loop);
    if (flow == StepFlow::kEndTurn) break;
    if (flow == StepFlow::kRetryStep) {
      --loop.step;
      continue;
    }

    const json& schemas = available_schemas_.Schemas();
    // The step's note is the request's last message and leaves with the
    // answer, so a nudge never accumulates in history.
    const bool noted = !loop.note.empty();
    if (noted) {
      conversation_.Push(HarnessMessage(std::exchange(loop.note, {})),
                         MessageKind::kInternal);
    }
    ChatResult response = Chat("turn", loop.step, schemas);
    if (noted) {
      conversation_.Erase(conversation_.Size() - 1, conversation_.Size());
    }
    flow = HandleFailedResponse(response, state, loop, schemas, attachment);
    if (flow == StepFlow::kEndTurn) break;
    if (flow == StepFlow::kNextStep) continue;
    if (flow == StepFlow::kRetryStep) {
      --loop.step;
      continue;
    }

    RecordModelResponse(response, state, loop.tool_counts);
    if (TurnTokenBudgetExceeded(state)) break;
    if (TurnCostExceeded(state)) break;
    if (response.stop_cause == ResponseStopCause::kPause &&
        !response.replay.empty()) {
      constexpr int64_t kProviderContinuationLimit = 8;
      if (++loop.provider_continuations > kProviderContinuationLimit) {
        FailTurn(state, "provider continuation limit (8) reached");
        break;
      }
      PushAssistantMessage(response, {});
      DebugLog("provider_continuation",
               {{"turn", turn_id_},
                {"step", loop.step},
                {"wire_api", WireApiName(api_.capabilities.wire_api)}});
      continue;
    }

    std::vector<ToolCall> calls = std::move(response.tool_calls);

    flow = HandleResponseStop(response, calls.size(), state, loop);
    if (flow == StepFlow::kEndTurn) break;
    if (flow == StepFlow::kNextStep) continue;

    // A call without a function name was dropped: the rest of the response
    // stands, and the model hears about the one that did not run.
    if (response.nameless_tool_calls > 0) {
      if (!Strike(Fault::kNameless, state, loop)) break;
      if (calls.empty()) continue;
    }
    if (calls.empty() && (ContainsForeignToolCallMarkup(response.content) ||
                          response.suppressed)) {
      if (!Strike(Fault::kMarkup, state, loop)) break;
      continue;
    }
    if (!ToolCallsWithinLimits(calls, state, loop)) {
      break;
    }
    RecordToolRoundRepetition(calls, loop);
    if (calls.empty() && response.content.empty()) {
      if (!Strike(Fault::kEmpty, state, loop,
                  state.metrics.tool_count > 0
                      ? "Return the final answer from existing results. Do "
                        "not repeat completed work."
                      : "The previous reply arrived empty. Answer the "
                        "request directly.")) {
        break;
      }
      Emit(NoticeEvent(PresentationStatus::kNeutral,
                       "recovering empty response"));
      continue;
    }

    PushAssistantMessage(response, calls);
    flow = calls.empty() ? FinishWithProse(state, loop)
                         : ExecuteToolCalls(calls, state, loop);
    if (flow == StepFlow::kEndTurn) break;
  }
  FinishTurn(state, loop.step);
}

void Agent::FinishTurn(TurnExecution& state, int64_t step) {
  // Side routes may finish after the last model round. Account them before
  // deciding the terminal reason and constructing caller-visible metadata.
  AccountSideUsage(&state.metrics.usage);
  if (state.stop.reason == TurnStopReason::kNone &&
      state.stop.outcome != TurnOutcome::kComplete) {
    if (!TurnTokenBudgetExceeded(state)) TurnCostExceeded(state);
  }
  bool step_limited =
      state.limits.max_steps > 0 && step >= state.limits.max_steps;
  if (step_limited && state.stop.reason == TurnStopReason::kNone) {
    last_error_ =
        "step limit (" + std::to_string(state.limits.max_steps) + ") reached";
    state.stop.reason = TurnStopReason::kMaxSteps;
    Emit(NoticeEvent(PresentationStatus::kFailed,
                     last_error_ + " — stopping this turn"));
  }
  // One record of why this turn ended and what was in force, so a parent
  // reading a child's envelope can tell "raise this ceiling and retry" from
  // "the work is done" without parsing prose.
  int64_t steps_used = step_limited ? state.limits.max_steps : step + 1;
  std::string reason = TurnStopReasonName(state.stop.reason);
  if (reason.empty()) {
    switch (state.stop.outcome) {
      case TurnOutcome::kComplete:
        reason = "completed";
        break;
      case TurnOutcome::kInterrupted:
        Emit(NoticeEvent(PresentationStatus::kWarned, "interrupted"));
        reason = "cancelled";
        break;
      case TurnOutcome::kError:
        reason = "error";
        break;
      case TurnOutcome::kStepLimit:
      case TurnOutcome::kBudgetExceeded:
        reason = TurnOutcomeName(state.stop.outcome);
        break;
    }
  }
  last_stop_ = {{"reason", reason},
                {"detail", last_error_},
                {"steps", steps_used},
                {"tool_calls", state.metrics.tool_count},
                {"generated_tokens", state.metrics.usage.GeneratedTokens()},
                {"cost", state.metrics.usage.cost},
                {"limits",
                 {{"max_steps", state.limits.max_steps},
                  {"max_tool_calls", state.limits.max_tool_calls},
                  {"max_turn_seconds", state.limits.max_turn_seconds},
                  {"max_turn_tokens", state.limits.max_turn_tokens},
                  {"session_token_budget", state.limits.session_token_budget},
                  {"max_turn_cost", state.limits.max_turn_cost},
                  {"session_budget", state.limits.session_budget}}},
                {"session_generated_tokens", session_usage_.GeneratedTokens()},
                {"session_cost", session_usage_.cost}};
  PruneAttachments(state.start);
  ArchiveTurnTrace(state.start);
  PruneOldToolResults();

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              state.started)
                    .count();
  double tokens_per_second =
      state.metrics.model_generation_ms > 0
          ? static_cast<double>(state.metrics.model_generated_tokens) * 1000.0 /
                static_cast<double>(state.metrics.model_generation_ms)
          : 0;
  const int64_t direct_model_calls = std::max(
      int64_t{0},
      JsonValue(conversation_.Statistics(), "model_calls", int64_t{0}) -
          state.model_calls_before);
  const int64_t background_model_calls =
      JsonValue(turn_side_statistics_, "model_calls", int64_t{0});
  const int64_t background_tool_calls =
      JsonValue(turn_side_statistics_, "tool_calls", int64_t{0});
  conversation_.AddStatistics({{"recorded_turns", 1},
                               {"tool_calls", state.metrics.tool_count},
                               {"duration_ms", secs * 1000}});
  json summary = {
      {"turn", turn_id_},
      {"turn_root", turn_root_},
      {"route", RouteSelection(api_, LoadProviderCatalog().providers)},
      {"outcome", TurnOutcomeName(state.stop.outcome)},
      {"steps", steps_used},
      {"tool_calls", state.metrics.tool_count + background_tool_calls},
      {"direct_tool_calls", state.metrics.tool_count},
      {"model_calls", direct_model_calls + background_model_calls},
      {"direct_model_calls", direct_model_calls},
      {"duration_ms", secs * 1000},
      {"ttt_ms", state.metrics.ttt_ms},
      {"tokens_per_second", tokens_per_second},
      {"generation_ms", state.metrics.model_generation_ms},
      {"generated_tokens", state.metrics.model_generated_tokens},
      {"usage_reported",
       state.metrics.usage_reported || HasUsage(state.metrics.usage)},
      {"usage", UsageJson(state.metrics.usage)}};
  if (!turn_side_statistics_.empty()) {
    summary["background_statistics"] = turn_side_statistics_;
  }
  if (json files = edits_.Files(turn_id_); !files.empty()) {
    summary["files"] = std::move(files);
  }
  // The stored block already carries the full summary: the footer the live
  // turn prints and the one --resume replays read identical inputs.
  summary.update({{"session_usage", UsageJson(session_usage_)},
                  {"messages", conversation_.Size()},
                  {"context_tokens", ContextUsed()},
                  {"line_open", state.line_open}});
  json block = conversation_.RecordEntry({{"kind", "turn_summary"},
                                          {"turn_root", turn_root_},
                                          {"summary", summary}});
  Emit(Event{EventId::kMessageChanged, {{"block", std::move(block)}}});
  Event completed{EventId::kTurnCompleted, std::move(summary)};
  completed.render = true;
  Emit(std::move(completed));
  active_deadline_ = std::chrono::steady_clock::time_point::max();
  PublishSideContext();
}

}  // namespace uagent
