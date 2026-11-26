// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent/agent.h"
#include "include/agent/node.h"
#include "include/agent/protocol.h"
#include "include/agent/session_role.h"
#include "include/agent/transcript.h"
#include "include/api/citations.h"
#include "include/api/retry.h"
#include "include/core/checked.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/providers/providers.h"
#include "src/agent/turn_internal.h"

namespace uagent {
namespace {
std::string NameList(const std::vector<std::string>& names) {
  std::string list;
  for (const std::string& name : names) {
    list += (list.empty() ? "" : ", ") + name;
  }
  return list;
}
}  // namespace

void Agent::RecordModelResponse(
    ChatResult& response, TurnExecution& state,
    std::unordered_map<std::string, int64_t>& tool_counts) {
  Usage response_usage = AccountModelUsage(response.usage);
  state.metrics.usage_reported =
      state.metrics.usage_reported ||
      (response.usage.is_object() && !response.usage.empty());
  // The turn's TTFT is its first streamed request's: counted from the turn's
  // start it would also hold approvals and tool runs before that request.
  if (state.metrics.ttt_ms < 0 && response.first_token_ms >= 0) {
    state.metrics.ttt_ms = response.first_token_ms;
  }
  if ((state.limits.max_turn_tokens > 0 ||
       state.limits.session_token_budget > 0) &&
      (!response.usage.is_object() || response.usage.empty()) &&
      !token_warning_shown_) {
    token_warning_shown_ = true;
    Emit(NoticeEvent(PresentationStatus::kWarned,
                     "provider does not report usage; token budget is not "
                     "enforceable"));
    DebugLog("token_usage_unavailable", {{"route", ActiveRoute()}});
  }
  // Dollar limits read reported cost: the session's budget.
  if (response.usage.is_object() && !response_usage.cost_reported &&
      !cost_warning_shown_ && state.limits.session_budget > 0) {
    cost_warning_shown_ = true;
    Emit(NoticeEvent(PresentationStatus::kWarned,
                     "provider does not report cost; dollar limits are not "
                     "enforceable"));
    DebugLog("cost_unavailable", {{"route", ActiveRoute()}});
  }
  // Hidden thinking generates tokens before the first visible event, so only
  // the full call duration cannot overstate the rate.
  if (response.duration_ms > 0 && response_usage.GeneratedTokens() > 0) {
    state.metrics.model_generation_ms += response.duration_ms;
    state.metrics.model_generated_tokens = SaturatingNonnegativeAdd(
        state.metrics.model_generated_tokens, response_usage.GeneratedTokens());
  }
  state.metrics.usage.Merge(response_usage);
  tool_counts["web_search"] = SaturatingNonnegativeAdd(
      tool_counts["web_search"], response_usage.web_searches);
  std::string citations = CitationMarkdown(response.annotations);
  if (response_usage.web_searches > 0 || !citations.empty()) {
    Event sources{EventId::kResponseSources,
                  {{"searches", response_usage.web_searches},
                   {"annotations", response.annotations},
                   {"citations", !response.content.empty()}}};
    Emit(std::move(sources));
  }
  if (!citations.empty() && !response.content.empty()) {
    response.content += citations;
  }
}

// An interrupted or failed model call; kProceed means the response is usable.
Agent::StepFlow Agent::HandleFailedResponse(ChatResult& response,
                                            TurnExecution& state,
                                            StepState& loop,
                                            const json& schemas,
                                            bool attachment) {
  // What the provider counted before a call failed is spent all the same.
  if (response.interrupted || !response.error.empty()) {
    state.metrics.usage.Merge(AccountModelUsage(response.usage));
  }
  if (response.interrupted) {
    conversation_.Push(
        HarnessMessage("(response interrupted; partial output was "
                       "discarded)"),
        MessageKind::kInternal);
    // Steering resumes the turn: apply it before recording anything, so a
    // resumed turn never carries the interrupt's outcome or error.
    if (ApplyQueuedSteering(loop)) return StepFlow::kNextStep;
    InterruptTurn(state);
    return StepFlow::kEndTurn;
  }
  if (response.error.empty()) return StepFlow::kProceed;
  if (TurnDeadlineExceeded(state)) return StepFlow::kEndTurn;
  if (!loop.context_overflow_recovery_attempted &&
      SafeContextRecovery(response) && !attachment) {
    loop.context_overflow_recovery_attempted = true;
    int64_t rejected_tokens =
        ContextTokens(RequestContextBytes(JsonEstimatedBytes(schemas)));
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
                     "provider context limit reached — compacting once"));
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

// Complete calls are salvaged only from a truncation; otherwise one bounded
// continuation keeps a partial response from being accepted as final.
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
  const std::string reason = response.finish_reason.empty()
                                 ? ResponseStopCauseName(cause)
                                 : response.finish_reason;
  // Only a cut by the output limit is known to have taken what follows.
  const bool by_length = cause == ResponseStopCause::kLength;
  const bool has_tool_calls = tool_call_count > 0;
  const bool salvage_calls =
      has_tool_calls && (cause == ResponseStopCause::kLength ||
                         cause == ResponseStopCause::kInputLimit);
  if (salvage_calls) {
    DebugLog("partial_response_salvaged", {{"turn", turn_id_},
                                           {"step", loop.step},
                                           {"reason", response.finish_reason},
                                           {"tool_calls", tool_call_count}});
    // Told with the results: a call the cut left unfinished is not there.
    if (by_length) PushStepNote(loop, FaultText(kCutCallNote, 0, reason));
    return StepFlow::kProceed;
  }

  // An unknown stop reason gets one retry even with calls, which are not
  // executed: a future policy stop must fail closed. A response cut off by
  // its length continues too.
  const bool continuable =
      cause == ResponseStopCause::kOther ||
      (!has_tool_calls && (cause == ResponseStopCause::kLength ||
                           cause == ResponseStopCause::kInputLimit));
  if (!continuable) {
    FailTurn(state, FaultText(kStoppedBeforeCompletion, 0, reason));
    return StepFlow::kEndTurn;
  }
  // Text goes on where it stopped. An output cut that took a call, or left
  // nothing to read, is told what did not run.
  const bool lost_more =
      by_length && (response.cut_tool_call || response.content.empty());
  if (!Strike(Fault::kCutOff, state, loop, reason, /*advise=*/!lost_more)) {
    return StepFlow::kEndTurn;
  }
  if (lost_more) PushStepNote(loop, FaultText(kCutCallNote, 0, reason));
  if (!response.content.empty()) PushAssistantMessage(response, {});
  Emit(NoticeEvent(PresentationStatus::kNeutral,
                   "continuing a partial model response"));
  return StepFlow::kNextStep;
}

void Agent::PushAssistantMessage(ChatResult& response,
                                 const std::vector<ToolCall>& calls) {
  json message = {{"role", "assistant"}, {"content", response.content}};
  if (!calls.empty()) {
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
  // Replay fields are kept while the turn continues; completed prose drops
  // them unless it holds what a provider-run tool did.
  if (!calls.empty() || response.stop_cause == ResponseStopCause::kPause ||
      ReplayHoldsHostedTool(response.replay)) {
    api_.PreserveAssistantReasoning(message, response);
  }
  conversation_.Push(std::move(message), MessageKind::kAssistant);
  // Readable thinking is display data; it never enters model requests.
  Usage usage;
  usage.Add(response.usage);
  json facts = {
      {"response_id", response.response_id},
      {"time", response.started_at.empty() ? UtcStamp() : response.started_at},
      {"route", RouteSelection(api_, LoadProviderCatalog().providers)},
      {"duration_ms", response.duration_ms},
      {"ttft_ms", response.first_token_ms},
      {"usage_reported", response.usage.is_object() && !response.usage.empty()},
      {"usage", UsageJson(usage)}};
  if (!response.served_by.empty()) facts["served_by"] = response.served_by;
  if (!response.generation_id.empty()) {
    facts["generation_id"] = response.generation_id;
  }
  if (response.duration_ms > 0 && usage.GeneratedTokens() > 0) {
    facts["tokens_per_second"] = static_cast<double>(usage.GeneratedTokens()) *
                                 1000 / response.duration_ms;
  }
  // Streamed as a step, it is published as one: never once as the answer.
  if (reply_as_step_) facts["held"] = true;
  conversation_.RecordDisplay(conversation_.LastDisplayId(), std::move(facts));
  // Only the preview the view shows is kept; more would crowd out other facts.
  conversation_.RecordDisplay(
      conversation_.LastDisplayId(),
      {{"reasoning", Utf8Trunc(response.reasoning, kPreviewChars)}});
  PublishMessage();
}

// Plain prose and no call: the turn is done unless steering reopened it.
Agent::StepFlow Agent::FinishWithProse(TurnExecution& state, StepState& loop) {
  // Mail that arrived during the final model call reopens the turn too; a
  // reply written as a step then stays one.
  DeliverMail(true);
  if (ApplyQueuedSteering(loop)) return StepFlow::kNextStep;
  if (!SteeringState().Requested()) {
    if (StepFlow held = HoldAnswer(state, loop); held != StepFlow::kProceed) {
      return held;
    }
  }
  // It stands as the answer.
  if (reply_as_step_) HoldReply(false);
  if (SteeringState().Requested()) return InterruptTurn(state);
  state.stop.outcome = TurnOutcome::kComplete;
  return StepFlow::kEndTurn;
}

void Agent::HoldReply(bool held) {
  conversation_.RecordDisplay(conversation_.LastDisplayId(), {{"held", held}});
  for (json& node : LastNodes(conversation_)) {
    Emit(Event{EventId::kMessageChanged, {{"node", std::move(node)}}});
  }
}

std::vector<std::string> Agent::AwaitAgents(
    const std::vector<std::string>& owed, const std::set<std::string>& calls,
    std::chrono::steady_clock::time_point until, bool on_mail) {
  const json row = RecordWorkNotice(
      {{"view",
        {{"verb", {kWaitingFor, kWaitedFor}}, {"target", NameList(owed)}}},
       {"state", "running"}});
  const std::string id = row["id"];
  // The status line says who the turn waits for.
  Emit(Event{
      EventId::kMessageChanged,
      {{"node", EntryNode(row)},
       {"waiting_for", std::string(kWaitingFor) + " " + NameList(owed)}}});
  std::vector<std::string> left = agent_wait_(calls, until, on_mail);
  conversation_.RecordDisplay(
      id, {{"state", AbortRequested() ? "cancelled" : "ok"}});
  Emit(Event{EventId::kMessageChanged,
             {{"node", EntryNode(JsonValue(conversation_.DisplayFacts(),
                                           id.c_str(), json::object()))},
              {"waiting_for", ""}}});
  return left;
}

// An answer given while agents this turn started still owe a report is held
// as a step of the work: the host waits for them, with no model call, and the
// model writes one answer with what arrived. Agents of earlier turns never
// hold it: their reports come with a turn of their own.
Agent::StepFlow Agent::HoldAnswer(TurnExecution& state, StepState& loop) {
  if (!agent_wait_ || state.agent_calls.empty()) return StepFlow::kProceed;
  const std::vector<std::string> owed =
      agent_wait_(state.agent_calls, {}, true);
  if (owed.empty()) {
    // One that ended since the mail was taken mailed its report first.
    DeliverMail(true);
    return ApplyQueuedSteering(loop) ? StepFlow::kNextStep : StepFlow::kProceed;
  }
  const std::string names = NameList(owed);
  if (loop.holds >= kAnswerHolds) {
    RecordWorkNotice({{"view",
                       {{"verb", {kAnsweredBefore, kAnsweredBefore}},
                        {"target", names + " reported"}}},
                      {"state", "ok"}});
    return StepFlow::kProceed;
  }
  ++loop.holds;
  HoldReply(true);
  const std::vector<std::string> left = AwaitAgents(
      owed, state.agent_calls,
      std::min(state.deadline, DeadlineAfter(AgentsWaitSeconds())), true);
  // Escape or the person's message ends the hold: the answer stands.
  if (AbortRequested() || SteeringState().Requested() ||
      SteeringYieldRequested()) {
    HoldReply(false);
    if (ApplyQueuedSteering(loop)) return StepFlow::kNextStep;
    return InterruptTurn(state);
  }
  ++work_.notes;
  if (left.size() < owed.size()) loop.holds = 0;
  // Kept after the held answer, so the history reads in order.
  conversation_.Push(
      HarnessMessage("[Your answer was held" +
                     (left.empty() ? " until " + names + " finished"
                                   : ": " + NameList(left) + " still working") +
                     ". Write one complete answer with what has arrived" +
                     (loop.holds == kAnswerHolds && !left.empty()
                          ? ", and say who has not reported."
                          : ".") +
                     "]"),
      MessageKind::kInternal);
  return StepFlow::kNextStep;
}

}  // namespace uagent
