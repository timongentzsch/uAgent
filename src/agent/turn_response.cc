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
#include "src/agent/turn_internal.h"

namespace uagent {
void Agent::RecordModelResponse(
    ChatResult& response, TurnExecution& state,
    std::unordered_map<std::string, int64_t>& tool_counts) {
  Usage response_usage = AccountModelUsage(response.usage);
  state.metrics.usage_reported =
      state.metrics.usage_reported ||
      (response.usage.is_object() && !response.usage.empty());
  if (state.metrics.ttt_ms < 0 && response.first_token_ms >= 0) {
    state.metrics.ttt_ms =
        std::max(0.0, std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - state.started)
                              .count() -
                          response.duration_ms) +
        response.first_token_ms;
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
  std::string citations = CitationMarkdown(response.annotations);
  if (response_usage.web_searches > 0 || !citations.empty()) {
    Event sources{EventId::kResponseSources,
                  {{"searches", response_usage.web_searches},
                   {"annotations", response.annotations},
                   {"line_open", state.line_open},
                   {"citations", !response.content.empty()}}};
    sources.verbose = verbose_;
    Emit(std::move(sources));
    state.line_open = false;
  }
  if (!citations.empty() && !response.content.empty()) {
    response.content += citations;
    state.line_open = false;
  }
}

// An interrupted or failed model call; kProceed means the response is usable.
Agent::StepFlow Agent::HandleFailedResponse(ChatResult& response,
                                            TurnExecution& state,
                                            StepState& loop,
                                            const json& schemas,
                                            bool attachment) {
  if (response.interrupted) {
    state.line_open = false;
    printf("\n");
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
    PushAssistantMessage(response, {});
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
  // Preserve the replay fields the active route actually emitted while the
  // turn continues; completed prose does not burden later turns.
  if (!calls.empty() || response.stop_cause == ResponseStopCause::kPause) {
    api_.PreserveAssistantReasoning(message, response);
  }
  conversation_.Push(std::move(message), MessageKind::kAssistant);
  // Readable thinking is display data even when the provider needs no replay
  // field on a completed prose message. It never enters model requests.
  Usage usage;
  usage.Add(response.usage);
  json facts = {
      {"response_id", response.response_id},
      {"content_revision", 1},
      {"content_complete", true},
      {"text_bytes", response.content.size()},
      {"time", response.started_at.empty() ? UtcStamp() : response.started_at},
      {"route", RouteSelection(api_, LoadProviderCatalog().providers)},
      {"duration_ms", response.duration_ms},
      {"ttft_ms", response.first_token_ms},
      {"usage_reported", response.usage.is_object() && !response.usage.empty()},
      {"usage", UsageJson(usage)}};
  if (response.duration_ms > 0 && usage.GeneratedTokens() > 0) {
    facts["tokens_per_second"] = static_cast<double>(usage.GeneratedTokens()) *
                                 1000 / response.duration_ms;
  }
  conversation_.RecordDisplay(conversation_.LastDisplayId(), std::move(facts));
  conversation_.RecordDisplay(
      conversation_.LastDisplayId(),
      {{"reasoning", Utf8Trunc(response.reasoning, size_t{48} * 1024)},
       {"reasoning_available", !response.reasoning.empty()},
       {"reasoning_revision", 1},
       {"reasoning_complete", true},
       {"reasoning_bytes", response.reasoning.size()}});
  PublishMessage();
}

// Plain prose and no call: the turn is done unless steering reopened it.
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

}  // namespace uagent
