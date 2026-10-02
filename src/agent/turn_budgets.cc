// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "src/agent/turn_internal.h"

namespace uagent {
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

void Agent::Advise(Fault fault, StepState& loop, const std::string& about) {
  const int64_t strike = loop.recovery.strikes[static_cast<size_t>(fault)];
  for (const auto& advice : kFaultRules[static_cast<size_t>(fault)].advice) {
    if (advice.note && (advice.at == 0 || advice.at == strike)) {
      PushStepNote(loop, FaultText(advice.note, strike, about));
    }
  }
}

bool Agent::Strike(Fault fault, TurnExecution& state, StepState& loop,
                   const std::string& about, bool advise) {
  const FaultRule& rule = kFaultRules[static_cast<size_t>(fault)];
  const int64_t strike = ++loop.recovery.strikes[static_cast<size_t>(fault)];
  const bool stop = rule.stop_after > 0 && strike >= rule.stop_after;
  DebugLog("model_fault", {{"turn", turn_id_},
                           {"step", loop.step},
                           {"fault", rule.name},
                           {"strike", strike},
                           {"stopped", stop}});
  if (!stop) {
    if (advise) Advise(fault, loop, about);
    return true;
  }
  std::string message = FaultText(rule.stopped, strike, about);
  if (rule.stop_reason == TurnStopReason::kNone) {
    FailTurn(state, std::move(message));
  } else {
    FailBudget(state, rule.stop_reason, std::move(message));
  }
  return false;
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

bool Agent::ToolCallsWithinLimits(const std::vector<ToolCall>& calls,
                                  TurnExecution& state, StepState& loop) {
  if (calls.empty()) return true;
  const int64_t max_tool_calls = state.limits.max_tool_calls;
  std::string& last_call = loop.recovery.last_call;
  int64_t& repeated_calls =
      loop.recovery.strikes[static_cast<size_t>(Fault::kRepeat)];
  if (max_tool_calls > 0 &&
      state.metrics.tool_count + static_cast<int64_t>(calls.size()) >
          max_tool_calls) {
    FailBudget(
        state, TurnStopReason::kMaxToolCalls,
        "tool call limit reached (" + std::to_string(max_tool_calls) + ")");
    return false;
  }
  // Valid repetition is counted here and advised on by ExecuteToolCalls once
  // the results are in; deterministic schema/policy rejections use a separate
  // lower bound because the same request cannot start succeeding.
  for (const ToolCall& call : calls) {
    const Tool* tool = FindTool(tools_, call.name);
    json arguments = json::parse(call.args, nullptr, false);
    if (tool) CanonicalizeToolArguments(*tool, arguments);
    bool blocking_wait = tool && ToolCallBlocks(*tool, arguments);
    if (blocking_wait) {
      last_call.clear();
      repeated_calls = 0;
      continue;
    }
    std::string normalized =
        arguments.is_object() ? JsonDump(arguments) : call.args;
    std::string signature = call.name + "\n" + normalized;
    if (signature != last_call) repeated_calls = 0;
    last_call = std::move(signature);
    if (!Strike(Fault::kRepeat, state, loop, "", /*advise=*/false)) {
      return false;
    }
  }
  return true;
}

// Worth a trace record long before it is worth stopping the turn.
void Agent::RecordToolRoundRepetition(const std::vector<ToolCall>& calls,
                                      StepState& loop) {
  if (calls.size() != 1) {
    loop.recovery.last_single_tool.clear();
    loop.recovery.same_tool_rounds = 0;
    return;
  }
  loop.recovery.same_tool_rounds =
      calls[0].name == loop.recovery.last_single_tool
          ? loop.recovery.same_tool_rounds + 1
          : 1;
  loop.recovery.last_single_tool = calls[0].name;
  if (loop.recovery.same_tool_rounds == kRepeatedToolRoundTraceAfter) {
    DebugLog("repeated_tool_rounds",
             {{"turn", turn_id_},
              {"step", loop.step},
              {"tool", calls[0].name},
              {"rounds", loop.recovery.same_tool_rounds}});
  }
}

bool Agent::StopForRepeatedRejections(
    const std::vector<ToolRejection>& rejections, TurnExecution& state,
    StepState& loop) {
  std::unordered_set<std::string> seen_this_round;
  for (const ToolRejection& rejection : rejections) {
    std::string key = rejection.tool + "\n" + rejection.issue_code + "\n" +
                      rejection.issue_field + "\n" + rejection.operation;
    if (!seen_this_round.insert(key).second) continue;
    int64_t rounds = ++loop.recovery.rejection_rounds[key];
    if (rounds < kRejectedCallStopAfter) continue;

    std::string message = "model repeated an equivalent rejected " +
                          rejection.tool + " call " +
                          std::to_string(kRejectedCallStopAfter) + " times (" +
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

}  // namespace uagent
