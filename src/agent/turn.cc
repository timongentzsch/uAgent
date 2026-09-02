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
#include "include/tools/jobs.h"
#include "src/agent/turn_internal.h"

namespace uagent {

namespace {

bool GenericSessionTitle(std::string title) {
  title = AsciiLower(Trim(title));
  return title == "hi" || title == "hello" || title == "hey" || title == "test";
}

}  // namespace

// Every ordinary turn failure records the same terminal state and notice.
void Agent::FailTurn(TurnExecution& state, std::string message) {
  last_error_ = std::move(message);
  state.stop.outcome = TurnOutcome::kError;
  Emit(NoticeEvent(PresentationStatus::kFailed, last_error_));
}

// Every bound the turn enforces ends the same way: record why, mark the turn,
// and say so in red.
void Agent::FailBudget(TurnExecution& state, TurnStopReason reason,
                       std::string message) {
  last_error_ = std::move(message);
  state.stop.outcome = TurnOutcome::kBudgetExceeded;
  state.stop.reason = reason;
  Emit(NoticeEvent(PresentationStatus::kFailed, last_error_));
}

bool Agent::TurnDeadlineExceeded(TurnExecution& state,
                                 std::chrono::seconds reserve) {
  if (std::chrono::steady_clock::now() + reserve < state.deadline) return false;
  FailBudget(state, TurnStopReason::kTurnDeadline,
             "turn time limit reached (" +
                 std::to_string(state.limits.max_turn_seconds) + "s)");
  return true;
}

bool Agent::TurnTokenBudgetExceeded(TurnExecution& state, bool before_model) {
  int64_t limit = state.limits.max_turn_tokens;
  int64_t spent = state.metrics.usage.GeneratedTokens();
  std::string scope = "turn";
  const int64_t session_spent = session_usage_.GeneratedTokens();
  const bool session_limited = state.limits.session_token_budget > 0;
  const bool session_exhausted =
      session_limited &&
      (before_model ? session_spent >= state.limits.session_token_budget
                    : session_spent > state.limits.session_token_budget);
  if (session_exhausted) {
    limit = state.limits.session_token_budget;
    spent = session_spent;
    scope = "session";
  }
  if (limit <= 0 || (before_model ? spent < limit : spent <= limit)) {
    return false;
  }
  FailBudget(state,
             scope == "session" ? TurnStopReason::kSessionTokenBudget
                                : TurnStopReason::kTurnTokens,
             scope + " generated-token limit " +
                 (spent == limit ? "reached (" : "exceeded (") +
                 FmtCount(limit) + ")");
  return true;
}

bool Agent::TurnCostExceeded(TurnExecution& state) {
  double limit = state.limits.max_turn_cost;
  double spent = state.metrics.usage.cost;
  std::string scope = "turn";
  if (state.limits.session_budget > 0 &&
      session_usage_.cost > state.limits.session_budget) {
    limit = state.limits.session_budget;
    spent = session_usage_.cost;
    scope = "session";
  }
  if (limit <= 0 || spent <= limit) return false;
  FailBudget(state,
             scope == "session" ? TurnStopReason::kSessionBudget
                                : TurnStopReason::kTurnCost,
             scope + " cost limit exceeded (" + FmtCost(limit) + ")");
  return true;
}

void Agent::RecordModelResponse(
    ChatResult& response, TurnExecution& state,
    std::unordered_map<std::string, int64_t>& tool_counts) {
  Usage response_usage = AccountModelUsage(response.usage);
  if (state.metrics.ttt_ms < 0 && response.first_event_ms >= 0) {
    state.metrics.ttt_ms = response.first_event_ms;
  }
  if ((state.limits.max_turn_tokens > 0 ||
       state.limits.session_token_budget > 0) &&
      (!response.usage.is_object() || response.usage.empty()) &&
      !token_warning_shown_) {
    token_warning_shown_ = true;
    Emit(NoticeEvent(PresentationStatus::kWarned,
                     "· provider does not report usage; token budget is not "
                     "enforceable"));
    DebugLog("token_usage_unavailable", {{"route", ActiveRoute()}});
  }
  if (state.limits.session_budget > 0 && response.usage.is_object() &&
      !response_usage.cost_reported && !cost_warning_shown_) {
    cost_warning_shown_ = true;
    Emit(NoticeEvent(PresentationStatus::kWarned,
                     "· provider does not report cost; dollar budget is not "
                     "enforceable"));
    DebugLog("cost_unavailable", {{"route", ActiveRoute()}});
  }
  // Tokens are routinely generated before the first visible event (hidden
  // thinking, tool-call deliberation) and providers do not always report them
  // as reasoning, so only the full call duration cannot overstate the rate.
  if (response.duration_ms > 0 && response_usage.GeneratedTokens() > 0) {
    state.metrics.model_generation_ms += response.duration_ms;
    state.metrics.model_generated_tokens = SaturatingNonnegativeAdd(
        state.metrics.model_generated_tokens, response_usage.GeneratedTokens());
  }
  state.metrics.usage.Merge(response_usage);
  tool_counts["web_search"] = SaturatingNonnegativeAdd(
      tool_counts["web_search"], response_usage.web_searches);
  turn_search_trace_.Add(response_usage.web_searches, response.annotations);
  state.line_open = !response.suppressed && !response.content.empty() &&
                    response.content.back() != '\n';
  if (PrintSearchReceipt(response_usage.web_searches, response.annotations,
                         verbose_, state.line_open)) {
    state.line_open = false;
  }
  std::string citations = CitationMarkdown(response.annotations);
  if (!citations.empty() && !response.content.empty()) {
    PrintCitationSources(response.annotations);
    response.content += citations;
    state.line_open = false;
  }
}

bool Agent::ToolCallsWithinLimits(const std::vector<ToolCall>& calls,
                                  TurnExecution& state, int64_t max_tool_calls,
                                  std::string& last_call,
                                  int64_t& repeated_calls) {
  if (calls.empty()) return true;
  if (max_tool_calls > 0 &&
      state.metrics.tool_count + static_cast<int64_t>(calls.size()) >
          max_tool_calls) {
    FailBudget(
        state, TurnStopReason::kMaxToolCalls,
        "tool call limit reached (" + std::to_string(max_tool_calls) + ")");
    return false;
  }
  bool repeated = false;
  for (const ToolCall& call : calls) {
    const Tool* tool = FindTool(tools_, call.name);
    json arguments = json::parse(call.args, nullptr, false);
    if (tool) CanonicalizeToolArguments(*tool, arguments);
    bool blocking_wait =
        tool && tool->blocking_wait_default_ms >= 0 &&
        JsonValue(arguments, "wait_ms", tool->blocking_wait_default_ms) > 0;
    if (blocking_wait) {
      last_call.clear();
      repeated_calls = 0;
      continue;
    }
    std::string normalized =
        arguments.is_object() ? JsonDump(arguments) : call.args;
    std::string signature = call.name + "\n" + normalized;
    repeated_calls = signature == last_call ? repeated_calls + 1 : 1;
    last_call = std::move(signature);
    repeated = repeated || repeated_calls > 3;
  }
  if (!repeated) return true;
  FailBudget(state, TurnStopReason::kRepeatedCalls,
             "model repeated the same tool call more than 3 times");
  return false;
}

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
  std::vector<std::string> queued = SteeringState().TakeQueued();
  if (queued.empty()) return false;
  SteeringState().Take();
  for (std::string& input : queued) {
    for (std::string& skill : ExplicitSkillContext(input)) {
      PushSkillContext(std::move(skill));
    }
    conversation_.Push({{"role", "user"}, {"content", std::move(input)}},
                       MessageKind::kUser);
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
Agent::StepFlow Agent::PrepareStep(TurnExecution& state, StepState& loop,
                                   json& schemas) {
  ApplyQueuedSteering(loop);
  RefreshSystemMessage();
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
  schemas =
      AvailableToolSchemas(tools_, schemas_, loop.tool_counts, availability);
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
Agent::StepFlow Agent::HandleFailedResponse(ChatResult& response,
                                            TurnExecution& state,
                                            StepState& loop,
                                            const json& schemas,
                                            bool attachment) {
  if (response.interrupted) {
    state.line_open = false;
    // Recorded before the steering check: if steering resumes the turn, the
    // outcome is overwritten by whatever ends it.
    InterruptTurn(state);
    printf("\n");
    Emit(NoticeEvent(PresentationStatus::kWarned, "· interrupted"));
    conversation_.Push(
        HarnessMessage("(response interrupted; partial output was "
                       "discarded)"),
        MessageKind::kInternal);
    return ApplyQueuedSteering(loop) ? StepFlow::kNextStep : StepFlow::kEndTurn;
  }
  if (response.error.empty()) return StepFlow::kProceed;
  state.line_open = false;
  if (TurnDeadlineExceeded(state)) return StepFlow::kEndTurn;
  if (!loop.context_overflow_recovery_attempted &&
      SafeContextRecovery(response) && !attachment) {
    loop.context_overflow_recovery_attempted = true;
    int64_t rejected_tokens =
        EstimatedTokens(RequestContextBytes(JsonEstimatedBytes(schemas)));
    int64_t learned_context =
        std::max<int64_t>(4096, rejected_tokens - rejected_tokens / 10);
    int64_t prior_context = api_.ctx_window;
    if (prior_context <= 0 || learned_context < prior_context) {
      api_.ctx_window = learned_context;
      Emit(Event{EventId::kCapabilityChanged,
                 {{"feature", "context_window"},
                  {"from", prior_context},
                  {"to", learned_context},
                  {"reason", "provider_rejected_request"}}});
    }
    DebugLog("context_overflow_recovery", {{"turn", turn_id_},
                                           {"step", loop.step},
                                           {"rejected_tokens", rejected_tokens},
                                           {"learned_context", api_.ctx_window},
                                           {"messages", conversation_.Size()}});
    Emit(NoticeEvent(PresentationStatus::kNeutral,
                     "· provider context limit reached — compacting once"));
    loop.midturn_compaction_enabled = false;
    if (Compact(true, &state.metrics.usage)) {
      state.start = conversation_.Size();
      return StepFlow::kRetryStep;
    }
    FailTurn(state, response.error);
    return StepFlow::kEndTurn;
  }
  if (response.remote_error_kind == RemoteErrorKind::kContextLengthExceeded) {
    DebugLog("context_overflow_recovery_skipped",
             {{"turn", turn_id_},
              {"step", loop.step},
              {"already_attempted", loop.context_overflow_recovery_attempted},
              {"attachment", attachment},
              {"semantic_progress", response.semantic_progress}});
  }
  if (DegradeAndRetry(response)) return StepFlow::kRetryStep;
  FailTurn(state, response.error);
  return StepFlow::kEndTurn;
}

// Text that imitates a tool protocol but parses as nothing. One correction is
// worth sending; a second means the model will not recover.
Agent::StepFlow Agent::HandleUnparsedToolMarkup(TurnExecution& state,
                                                StepState& loop) {
  if (!loop.markup_recovered) {
    loop.markup_recovered = true;
    conversation_.Push(
        HarnessMessage("[invalid model tool markup] The attempted call was "
                       "not executed. Return prose using existing results; "
                       "do not imitate a tool protocol."),
        MessageKind::kInternal);
    loop.pending_note = conversation_.Size() - 1;
    DebugLog("foreign_tool_markup_recovery",
             {{"turn", turn_id_}, {"step", loop.step}});
    return StepFlow::kNextStep;
  }
  state.stop.outcome = TurnOutcome::kError;
  last_error_ = "model repeatedly returned invalid tool markup";
  return StepFlow::kEndTurn;
}

// A completion with no answer and no call carries nothing to react to, so the
// first one is replayed unchanged, a repeat earns a guiding note, and only a
// third ends the turn: a barren provider response must not cost the work this
// turn has already done.
Agent::StepFlow Agent::HandleEmptyResponse(const ChatResult& response,
                                           TurnExecution& state,
                                           StepState& loop) {
  constexpr int64_t kEmptyResponseAttempts = 3;
  if (++loop.empty_responses >= kEmptyResponseAttempts) {
    FailTurn(state, "model returned an empty response");
    return StepFlow::kEndTurn;
  }
  // The first replay goes out unchanged: only a repeat is evidence that the
  // model needs steering rather than another attempt.
  if (loop.empty_responses > 1) {
    conversation_.Push(
        HarnessMessage(state.metrics.tool_count > 0
                           ? "[empty model response] Return the final "
                             "answer from existing results. Do not "
                             "repeat completed work."
                           : "[empty model response] The previous reply "
                             "arrived empty. Answer the request "
                             "directly."),
        MessageKind::kInternal);
    loop.pending_note = conversation_.Size() - 1;
  }
  DebugLog("empty_response_recovery",
           {{"turn", turn_id_},
            {"step", loop.step},
            {"attempt", loop.empty_responses},
            {"guided", loop.empty_responses > 1},
            {"finish_reason", response.finish_reason}});
  Emit(
      NoticeEvent(PresentationStatus::kNeutral, "· recovering empty response"));
  return StepFlow::kNextStep;
}

// A provider stop is separate from transport success. Salvage complete calls
// only for truncation; otherwise one bounded continuation prevents a partial
// prose response or an unfamiliar stop reason from being accepted as final.
Agent::StepFlow Agent::HandleResponseStop(ChatResult& response,
                                          size_t tool_call_count,
                                          TurnExecution& state,
                                          StepState& loop) {
  const ResponseStopCause cause = response.stop_cause;
  if (cause == ResponseStopCause::kNone ||
      cause == ResponseStopCause::kComplete ||
      cause == ResponseStopCause::kPause) {
    return StepFlow::kProceed;
  }
  const bool has_tool_calls = tool_call_count > 0;
  const bool salvage_calls =
      has_tool_calls && (cause == ResponseStopCause::kLength ||
                         cause == ResponseStopCause::kInputLimit);
  if (salvage_calls) {
    DebugLog("partial_response_salvaged", {{"turn", turn_id_},
                                           {"step", loop.step},
                                           {"reason", response.finish_reason},
                                           {"tool_calls", tool_call_count}});
    return StepFlow::kProceed;
  }

  // Unknown provider reasons get one retry even when they accompanied calls.
  // Do not execute those calls: a future policy stop must fail closed, while
  // the retry gives a harmless new spelling a chance to complete normally.
  const bool continuable = (cause == ResponseStopCause::kOther) ||
                           (!response.content.empty() && !has_tool_calls &&
                            (cause == ResponseStopCause::kLength ||
                             cause == ResponseStopCause::kInputLimit));
  if (continuable && loop.stop_recoveries++ == 0) {
    PushAssistantMessage(response, {}, false);
    conversation_.Push(
        HarnessMessage("[partial model response: " + response.finish_reason +
                       "] Continue exactly where the response stopped. Do "
                       "not repeat completed content or work."),
        MessageKind::kInternal);
    loop.pending_note = conversation_.Size() - 1;
    DebugLog("partial_response_continuation",
             {{"turn", turn_id_},
              {"step", loop.step},
              {"reason", response.finish_reason},
              {"content_chars", response.content.size()}});
    Emit(NoticeEvent(PresentationStatus::kNeutral,
                     "· continuing a partial model response"));
    return StepFlow::kNextStep;
  }

  std::string reason = response.finish_reason.empty()
                           ? ResponseStopCauseName(cause)
                           : response.finish_reason;
  FailTurn(state, "model response stopped before completion (" + reason + ")");
  return StepFlow::kEndTurn;
}

// Worth a trace record long before it is worth stopping the turn.
void Agent::RecordToolRoundRepetition(const std::vector<ToolCall>& calls,
                                      StepState& loop) {
  if (calls.size() != 1) {
    loop.last_single_tool.clear();
    loop.same_tool_rounds = 0;
    return;
  }
  loop.same_tool_rounds =
      calls[0].name == loop.last_single_tool ? loop.same_tool_rounds + 1 : 1;
  loop.last_single_tool = calls[0].name;
  if (loop.same_tool_rounds == 8) {
    DebugLog("repeated_tool_rounds", {{"turn", turn_id_},
                                      {"step", loop.step},
                                      {"tool", calls[0].name},
                                      {"rounds", loop.same_tool_rounds}});
  }
}

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

bool Agent::StopForRepeatedRejections(
    const std::vector<ToolRejection>& rejections, TurnExecution& state,
    StepState& loop) {
  constexpr int64_t kRejectedRoundLimit = 3;
  std::unordered_set<std::string> seen_this_round;
  for (const ToolRejection& rejection : rejections) {
    std::string key = rejection.tool + "\n" + rejection.issue_code + "\n" +
                      rejection.issue_field + "\n" + rejection.operation;
    if (!seen_this_round.insert(key).second) continue;
    int64_t rounds = ++loop.rejection_rounds[key];
    if (rounds < kRejectedRoundLimit) continue;

    std::string message = "model repeated an equivalent rejected " +
                          rejection.tool + " call 3 times (" +
                          rejection.issue_code;
    if (!rejection.issue_field.empty()) {
      message += ": " + rejection.issue_field;
    }
    message += ")";
    FailTurn(state, std::move(message));
    DebugLog("deterministic_rejection_loop",
             {{"turn", turn_id_},
              {"step", loop.step},
              {"tool", rejection.tool},
              {"issue_code", rejection.issue_code},
              {"issue_field", rejection.issue_field},
              {"operation", rejection.operation},
              {"rounds", rounds}});
    return true;
  }
  return false;
}

void Agent::PushAssistantMessage(ChatResult& response,
                                 const std::vector<ToolCall>& calls,
                                 bool text_mode) {
  json message = {{"role", "assistant"}, {"content", response.content}};
  if (!calls.empty() && !text_mode) {
    json encoded = json::array();
    for (const ToolCall& call : calls) {
      encoded.push_back(
          {{"id", call.id},
           {"type", "function"},
           {"function", {{"name", call.name}, {"arguments", call.args}}}});
    }
    message["tool_calls"] = std::move(encoded);
    // Tool-only turns carry no prose; store null so strict backends
    // (e.g. Anthropic) don't reject an empty text block on replay.
    if (response.content.empty()) message["content"] = nullptr;
  }
  // Preserve the replay fields the active route actually emitted while any
  // tool protocol continues; completed prose does not burden later turns.
  if (!calls.empty() || response.stop_cause == ResponseStopCause::kPause) {
    api_.PreserveAssistantReasoning(message, response);
  }
  conversation_.Push(std::move(message), MessageKind::kAssistant);
}

// Plain prose and no call: the turn is done unless steering reopened it.
Agent::StepFlow Agent::FinishWithProse(ChatResult& response,
                                       TurnExecution& state, StepState& loop) {
  // Content that looked like a tool call was held back from the stream; if it
  // didn't parse into one, it's prose -- show it now.
  if (response.suppressed) {
    MdPrint(response.content);
    printf("\n");
  }
  if (ApplyQueuedSteering(loop)) return StepFlow::kNextStep;
  if (SteeringState().Requested()) return InterruptTurn(state);
  state.complete = true;
  state.stop.outcome = TurnOutcome::kComplete;
  return StepFlow::kEndTurn;
}

Agent::StepFlow Agent::ExecuteToolCalls(const std::vector<ToolCall>& calls,
                                        bool text_mode, TurnExecution& state,
                                        StepState& loop) {
  if (state.line_open) printf("\n");
  std::vector<ToolRejection> rejections;
  std::vector<ActivityPollResult> activity_polls;
  bool cancelled =
      RunCalls(calls, text_mode, state.metrics.tool_count, loop.tool_counts,
               loop.stable_arguments, loop.step, state.deadline,
               loop.consecutive_failed_tools, rejections, activity_polls);
  state.line_open = false;
  bool foreground_interrupted = SteeringState().Requested() || cancelled;
  bool steering_applied = ApplyQueuedSteering(loop);
  if (cancelled) BgCancelSubagents(processes_);
  if (foreground_interrupted) {
    if (steering_applied) return StepFlow::kNextStep;
    if (cancelled) {
      Emit(NoticeEvent(PresentationStatus::kWarned, "· interrupted"));
    }
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

void Agent::Turn(const std::string& user_input, json user_content) {
  if (!user_content.is_null()) ApplyImageFallbackToUserContent(user_content);
  api_.turn_started = std::chrono::steady_clock::now();
  last_error_.clear();
  ++turn_id_;
  ++revision_;
  ++total_user_turns_;
  std::string title = FirstLine(user_input);
  if (session_title_.empty() ||
      (GenericSessionTitle(session_title_) && title.size() >= 12 &&
       !GenericSessionTitle(title))) {
    session_title_ = std::move(title);
  }
  std::string local_time = LocalStamp();
  if (!conversation_.Empty()) {
    conversation_.Set(0, SysMsg(), MessageKind::kSystem);
    applied_system_revision_ =
        adaptive_system_ ? adaptive_system_->revision : 0;
  }
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
    Emit(Event{EventId::kTurnStopped,
               {{"turn", turn_id_},
                {"outcome", "steered_during_compaction"},
                {"steps", 0}}});
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
  turn_search_trace_.Reset();
  state.limits.max_steps = api_.config.max_steps;
  state.limits.max_tool_calls = api_.config.max_tool_calls;
  state.limits.max_turn_seconds = api_.config.max_turn_seconds;
  state.limits.max_turn_tokens = api_.config.max_turn_tokens;
  state.limits.session_token_budget = api_.config.session_token_budget;
  state.limits.max_turn_cost = api_.config.max_turn_cost;
  state.limits.session_budget = api_.config.session_budget;
  state.deadline =
      state.limits.max_turn_seconds > 0
          ? DeadlineAfter(state.started, state.limits.max_turn_seconds)
          : std::chrono::steady_clock::time_point::max();
  active_deadline_ = state.deadline;
  loop.detached_records_available = !DetachedRecords().empty();

  for (; state.limits.max_steps <= 0 || loop.step < state.limits.max_steps;
       ++loop.step) {
    json schemas;
    StepFlow flow = PrepareStep(state, loop, schemas);
    if (flow == StepFlow::kEndTurn) break;
    if (flow == StepFlow::kRetryStep) {
      --loop.step;
      continue;
    }

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
      PushAssistantMessage(response, {}, false);
      DebugLog("provider_continuation",
               {{"turn", turn_id_},
                {"step", loop.step},
                {"wire_api", WireApiName(api_.capabilities.wire_api)}});
      continue;
    }

    std::vector<ToolCall> calls = std::move(response.tool_calls);
    std::vector<ToolCall> text_calls;
    if (calls.empty()) text_calls = ParseTextToolCalls(response.content);
    // Recorded before the move below empties `text_calls`.
    const bool parsed_text_calls = !text_calls.empty();
    bool text_mode = !api_.capabilities.native_tools && parsed_text_calls;
    if (text_mode) calls = std::move(text_calls);

    flow = HandleResponseStop(response, calls.size(), state, loop);
    if (flow == StepFlow::kEndTurn) break;
    if (flow == StepFlow::kNextStep) continue;

    if (calls.empty() &&
        (parsed_text_calls || ContainsForeignToolCallMarkup(response.content) ||
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

    PushAssistantMessage(response, calls, text_mode);
    flow = calls.empty() ? FinishWithProse(response, state, loop)
                         : ExecuteToolCalls(calls, text_mode, state, loop);
    if (flow == StepFlow::kEndTurn) break;
  }
  FinishTurn(state, loop.step);
}

// The one-line accounting footer printed after every turn.
std::string Agent::TurnStatsLine(const TurnExecution& state, double seconds,
                                 double tokens_per_second) {
  std::ostringstream stats;
  stats << FmtCount(state.metrics.usage.input) << " in";
  if (state.metrics.usage.cache_read) {
    stats << " (+" << FmtCount(state.metrics.usage.cache_read) << " cached)";
  }
  if (state.metrics.usage.cache_write) {
    stats << " (+" << FmtCount(state.metrics.usage.cache_write)
          << " cache write)";
  }
  stats << " · " << FmtCount(state.metrics.usage.output) << " out";
  if (state.metrics.usage.reasoning) {
    stats << ' ' << ITAL() << "(+" << FmtCount(state.metrics.usage.reasoning)
          << " reasoning)" << ItalOff();
  }
  if (state.metrics.usage.cost > 0)
    stats << " · " << FmtCost(state.metrics.usage.cost);
  if (state.metrics.usage.web_searches) {
    stats << " · " << state.metrics.usage.web_searches << " search"
          << (state.metrics.usage.web_searches == 1 ? "" : "es");
  }
  if (state.metrics.tool_count) {
    stats << " · " << state.metrics.tool_count << " tool"
          << (state.metrics.tool_count == 1 ? "" : "s");
  }
  if (tokens_per_second > 0) {
    stats << " · " << std::fixed << std::setprecision(1) << tokens_per_second
          << " tok/s";
  }
  if (state.metrics.ttt_ms >= 0) {
    stats << " · first " << FmtDuration(state.metrics.ttt_ms / 1000.0);
  }
  stats << " · " << FmtDuration(seconds);
  return stats.str();
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
  // One write: the interactive composer repaints on every chunk it observes,
  // so a footer split across writes would redraw the input line mid-line.
  std::ostringstream footer;
  // Chrome, not an agent action: dim like the status row, leaving cyan as the
  // single accent for things the agent did.
  footer << (state.line_open ? "\n" : "") << RST() << DIM()
         << TurnStatsLine(state, secs, tokens_per_second) << RST() << '\n';
  // One write, as above: fputs of the assembled string, never a stream of
  // pieces the composer could repaint between.
  fputs(footer.str().c_str(), stdout);
  Emit(Event{EventId::kTurnCompleted,
             {{"turn", turn_id_},
              {"outcome", TurnOutcomeName(state.stop.outcome)},
              {"steps", steps_used},
              {"tool_calls", state.metrics.tool_count},
              {"duration_ms", secs * 1000},
              {"ttt_ms", state.metrics.ttt_ms},
              {"tokens_per_second", tokens_per_second},
              {"generation_ms", state.metrics.model_generation_ms},
              {"generated_tokens", state.metrics.model_generated_tokens},
              {"usage", UsageJson(state.metrics.usage)},
              {"session_usage", UsageJson(session_usage_)},
              {"messages", conversation_.Size()},
              {"context_tokens", ContextUsed()}}});
  active_deadline_ = std::chrono::steady_clock::time_point::max();
  api_.turn_started = {};
}

}  // namespace uagent
