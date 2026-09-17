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
#include "include/agent/protocol.h"
#include "include/agent/trace.h"
#include "include/api/citations.h"
#include "include/api/retry.h"
#include "include/core/checked.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/signals.h"
#include "include/core/skills.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/md.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
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
          "· skill " + skill.name + " unavailable: " + result.output));
      continue;
    }
    Emit(NoticeEvent(PresentationStatus::kNeutral,
                     "· using skill " + skill.name));
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
        conversation_.Push(
            {{"role", "user"}, {"content", std::move(content)}},
            attachment ? MessageKind::kAttachment : MessageKind::kUser);
        if (attachment && message.images.is_array() &&
            !message.images.empty()) {
          conversation_.RecordDisplay(conversation_.LastDisplayId(),
                                      {{"files", message.images}});
        }
        PublishMessage(message.request_id);
        continue;
      }
      DebugLog("steering_attachments_failed",
               {{"turn", turn_id_}, {"error", error}});
    }
    conversation_.Push({{"role", "user"}, {"content", std::move(input)}},
                       MessageKind::kUser);
    PublishMessage(message.request_id);
  }
  loop.last_call.clear();
  loop.repeated_calls = 0;
  loop.quiet_activity_id = 0;
  loop.quiet_activity_polls = 0;
  loop.quiet_activity_advisory_sent = false;
  loop.consecutive_failed_tools = 0;
  loop.last_single_tool.clear();
  loop.same_tool_rounds = 0;
  loop.stable_arguments.clear();
  loop.rejection_rounds.clear();
  loop.failure_advisory_sent = false;
  loop.markup_recovered = false;
  loop.empty_responses = 0;
  DebugLog("steering_applied",
           {{"turn", turn_id_}, {"messages", queued.size()}});
  return true;
}

// Everything that happens before the model call: steering, a refreshed system
// message, the budget gates, and the schemas this step is allowed to offer.
// Everything that happens before the model call: steering, a refreshed system
// message, the budget gates, and the schemas this step is allowed to offer.
Agent::StepFlow Agent::PrepareStep(TurnExecution& state, StepState& loop) {
  // A collaborator's parent can speak to it mid-run; the guidance arrives as
  // steering and is applied by the very next statement, so nothing it queues
  // can strand at the end of a headless turn.
  DrainCollaboratorMailIntoSteering();
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
  MergeSideUsage(state.metrics.usage);
  if (TurnTokenBudgetExceeded(state, /*before_model=*/true)) {
    return StepFlow::kEndTurn;
  }
  if (TurnCostExceeded(state)) return StepFlow::kEndTurn;
  ToolAvailability availability{
      .detached_terminal = processes_.PendingCount() > 0 ||
                           processes_.DetachedCount() > 0 ||
                           loop.detached_records_available,
  };
  const json& schemas =
      available_schemas_.Get(tools_, schemas_, loop.tool_counts, availability);
  if (loop.step > 0 && BoolSetting(Cfg("UAGENT_PRUNE_SUPERSEDED_READS"))) {
    PruneOldToolResults(ToolPruneMode::kSupersededReads);
  }
  if (loop.step > 0 && loop.midturn_compaction_enabled) {
    MidturnCompact compacted =
        MaybeCompactDuringTurn(schemas, state.metrics.usage, state.start);
    if (compacted != MidturnCompact::kNotNeeded) {
      loop.midturn_compaction_enabled = false;
      // A successful compaction rebuilt the history, so a recorded note
      // index no longer refers to its note; a failed one left history
      // exactly as it was, and the note still has to be retracted.
      if (compacted == MidturnCompact::kSucceeded) loop.pending_note.reset();
      return StepFlow::kRetryStep;
    }
  }
  return StepFlow::kProceed;
}

// An interrupted or failed model call; kProceed means the response is usable.
bool Agent::HandleActivityPollResults(
    const std::vector<ActivityPollResult>& polls, bool exclusive,
    TurnExecution& state, StepState& loop) {
  auto reset = [&] {
    loop.quiet_activity_id = 0;
    loop.quiet_activity_polls = 0;
    loop.quiet_activity_advisory_sent = false;
  };

  bool successful = false;
  for (const ActivityPollResult& poll : polls) successful |= poll.ok;
  if (successful) {
    loop.last_call.clear();
    loop.repeated_calls = 0;
  }
  if (!exclusive || polls.size() != 1) {
    if (!polls.empty()) reset();
    return false;
  }

  const ActivityPollResult& poll = polls.front();
  if (!poll.ok || poll.terminal || !poll.no_change) {
    reset();
    return false;
  }
  if (loop.quiet_activity_id != poll.id) {
    reset();
    loop.quiet_activity_id = poll.id;
  }
  ++loop.quiet_activity_polls;

  // A slow activity answers "(no new output)" honestly, so quiet polls steer
  // rather than end the turn that owns the work. The ceiling still bounds a
  // model that ignores every escalation: UAGENT_MAX_STEPS defaults to off.
  constexpr int64_t kAdviseAfter = 2;
  constexpr int64_t kDirectAfter = 4;
  constexpr int64_t kStopAfter = 12;
  if (loop.quiet_activity_polls >= kStopAfter) {
    FailTurn(state, "activity " + std::to_string(poll.id) +
                        " is still running, "
                        "but the model polled it " +
                        std::to_string(loop.quiet_activity_polls) +
                        " times without new output and without waiting on it");
    DebugLog("activity_poll_loop", {{"turn", turn_id_},
                                    {"step", loop.step},
                                    {"activity_id", poll.id},
                                    {"polls", loop.quiet_activity_polls}});
    return true;
  }
  if (loop.quiet_activity_polls == kAdviseAfter ||
      loop.quiet_activity_polls == kDirectAfter) {
    const bool mandatory = loop.quiet_activity_polls == kDirectAfter;
    std::string note = "[activity poll advisory] Activity " +
                       std::to_string(poll.id) +
                       " is still running and has "
                       "returned no new output " +
                       std::to_string(loop.quiet_activity_polls) + " times. ";
    note += mandatory ? "Stop polling it: this turn ends in an error if you "
                        "keep polling without waiting. Either issue one "
                        "activity call with operation=wait, mode=any and "
                        "wait_ms, or read the output it writes elsewhere, or "
                        "do independent work and revisit it later."
                      : "Do not poll it again immediately. If completion "
                        "blocks the next step, issue one bounded activity "
                        "call with operation=wait, mode=any, and wait_ms; "
                        "otherwise continue independent work.";
    conversation_.Push(HarnessMessage(note), MessageKind::kInternal);
    loop.pending_note = conversation_.Size() - 1;
    loop.quiet_activity_advisory_sent = true;
    DebugLog("activity_poll_advisory", {{"turn", turn_id_},
                                        {"step", loop.step},
                                        {"activity_id", poll.id},
                                        {"polls", loop.quiet_activity_polls},
                                        {"mandatory", mandatory}});
  }
  return false;
}

Agent::StepFlow Agent::ExecuteToolCalls(const std::vector<ToolCall>& calls,
                                        TurnExecution& state, StepState& loop) {
  if (state.line_open) printf("\n");
  std::vector<ToolRejection> rejections;
  std::vector<ActivityPollResult> activity_polls;
  bool cancelled =
      RunCalls(calls, state.metrics.tool_count, loop.tool_counts,
               loop.stable_arguments, loop.step, state.deadline,
               loop.consecutive_failed_tools, rejections, activity_polls);
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
  if (!loop.failure_advisory_sent && loop.consecutive_failed_tools >= 3) {
    loop.failure_advisory_sent = true;
    conversation_.Push(
        HarnessMessage("[tool failure advisory] Three consecutive tool "
                       "calls failed. Reassess the shared premise or "
                       "execution environment before trying another "
                       "variant; use existing evidence or a different "
                       "approach when possible."),
        MessageKind::kInternal);
    loop.pending_note = conversation_.Size() - 1;
    DebugLog("tool_failure_advisory",
             {{"turn", turn_id_},
              {"step", loop.step},
              {"consecutive_failures", loop.consecutive_failed_tools}});
  }
  // Do not start a network request with only curl's one-second granularity
  // left after tools. Report the owning turn budget instead of a misleading
  // transport timeout that cannot possibly be retried.
  if (TurnDeadlineExceeded(state, std::chrono::seconds(1))) {
    return StepFlow::kEndTurn;
  }
  return StepFlow::kNextStep;
}

void Agent::Turn(const std::string& user_input, json user_content, json images,
                 const std::string& request_id) {
  api_.turn_started = std::chrono::steady_clock::now();
  last_error_.clear();
  // A new turn owns its outcome: clients must never re-report the
  // previous turn's stop from a boundary publish before this one ends.
  last_stop_ = nullptr;
  ++turn_id_;
  turn_root_.clear();
  reply_to_.clear();
  reply_excerpt_.clear();
  ++revision_;
  ++total_user_turns_;
  std::string title = FirstLine(user_input);
  if (session_title_.empty() ||
      (!custom_title_ && GenericSessionTitle(session_title_) &&
       title.size() >= 12 && !GenericSessionTitle(title))) {
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
  if (ContextNeedsCompaction(pending_bytes, schema_chars_, pressure,
                             projected_tokens)) {
    DebugLog("auto_compact", {{"turn", turn_id_},
                              {"projected_pct", pressure},
                              {"projected_tokens", projected_tokens}});
    Compact(true);
  }
  if (SteeringState().Requested() && SteeringState().QueuedCount() == 0) {
    conversation_.Push(
        {{"role", "user"},
         {"content", attachment ? std::move(user_content) : json(user_input)}},
        attachment ? MessageKind::kAttachment : MessageKind::kUser);
    if (!images.empty()) {
      conversation_.RecordDisplay(conversation_.LastDisplayId(),
                                  {{"files", images}});
    }
    PublishMessage(request_id);
    Emit(NoticeEvent(PresentationStatus::kWarned, "· interrupted"));
    Emit(Event{EventId::kTurnStopped,
               {{"turn", turn_id_}, {"outcome", "interrupted"}, {"steps", 0}}});
    api_.turn_started = {};
    return;
  }
  EnsureRuntimeContext();
  TurnExecution state;
  StepState loop;
  state.start = conversation_.Size();  // user message and prune_* start
  for (std::string& skill : explicit_skills) PushSkillContext(std::move(skill));
  conversation_.Push(
      {{"role", "user"},
       {"content", attachment ? std::move(user_content) : json(user_input)}},
      attachment ? MessageKind::kAttachment : MessageKind::kUser);
  if (!images.empty()) {
    conversation_.RecordDisplay(conversation_.LastDisplayId(),
                                {{"files", images}});
  }
  PublishMessage(request_id);
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
    ChatResult response = Chat("turn", loop.step, schemas);
    if (loop.pending_note) {
      // The index was the tail when it was recorded. If history moved under
      // it anyway, erasing blind would drop a real message, so drop the note
      // instead and leave a trace of the contract having been broken.
      if (*loop.pending_note < conversation_.Size() &&
          conversation_.KindAt(*loop.pending_note) == MessageKind::kInternal) {
        conversation_.Erase(*loop.pending_note, *loop.pending_note + 1);
      } else {
        DebugLog("pending_note_stale", {{"turn", turn_id_},
                                        {"step", loop.step},
                                        {"index", *loop.pending_note},
                                        {"size", conversation_.Size()}});
      }
      loop.pending_note.reset();
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

    if (calls.empty() && (ContainsForeignToolCallMarkup(response.content) ||
                          response.suppressed)) {
      if (HandleUnparsedToolMarkup(state, loop) == StepFlow::kNextStep) {
        continue;
      }
      break;
    }
    if (!ToolCallsWithinLimits(calls, state, state.limits.max_tool_calls,
                               loop.last_call, loop.repeated_calls)) {
      break;
    }
    RecordToolRoundRepetition(calls, loop);
    if (calls.empty() && response.content.empty()) {
      if (HandleEmptyResponse(response, state, loop) == StepFlow::kNextStep) {
        continue;
      }
      break;
    }

    PushAssistantMessage(response, calls);
    flow = calls.empty() ? FinishWithProse(response, state, loop)
                         : ExecuteToolCalls(calls, state, loop);
    if (flow == StepFlow::kEndTurn) break;
  }
  FinishTurn(state, loop.step);
}

void Agent::FinishTurn(TurnExecution& state, int64_t step) {
  // Side routes may finish after the last model round. Account them before
  // deciding the terminal reason and constructing caller-visible metadata.
  MergeSideUsage(state.metrics.usage);
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
        Emit(NoticeEvent(PresentationStatus::kWarned, "· interrupted"));
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
  conversation_.AddStatistics({{"recorded_turns", 1},
                               {"tool_calls", state.metrics.tool_count},
                               {"duration_ms", secs * 1000}});
  json summary = {
      {"turn", turn_id_},
      {"turn_root", turn_root_},
      {"route", RouteSelection(api_, LoadProviderCatalog().providers)},
      {"outcome", TurnOutcomeName(state.stop.outcome)},
      {"steps", steps_used},
      {"tool_calls", state.metrics.tool_count},
      {"duration_ms", secs * 1000},
      {"ttt_ms", state.metrics.ttt_ms},
      {"tokens_per_second", tokens_per_second},
      {"generation_ms", state.metrics.model_generation_ms},
      {"generated_tokens", state.metrics.model_generated_tokens},
      {"usage_reported", state.metrics.usage_reported},
      {"usage", UsageJson(state.metrics.usage)}};
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
  api_.turn_started = {};
}

}  // namespace uagent
