// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_AGENT_TURN_INTERNAL_H_
#define UAGENT_SRC_AGENT_TURN_INTERNAL_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "include/agent.h"
#include "include/core/usage.h"

namespace uagent {

enum class TurnOutcome {
  kStepLimit,
  kBudgetExceeded,
  kInterrupted,
  kError,
  kComplete
};
enum class TurnStopReason {
  kNone,
  kTurnDeadline,
  kSessionTokenBudget,
  kTurnTokens,
  kSessionBudget,
  kTurnCost,
  kMaxToolCalls,
  kRepeatedCalls,
  kMaxSteps
};

inline const char* TurnOutcomeName(TurnOutcome outcome) {
  switch (outcome) {
    case TurnOutcome::kStepLimit:
      return "step_limit";
    case TurnOutcome::kBudgetExceeded:
      return "budget_exceeded";
    case TurnOutcome::kInterrupted:
      return "interrupted";
    case TurnOutcome::kError:
      return "error";
    case TurnOutcome::kComplete:
      return "complete";
  }
  return "error";
}

inline const char* TurnStopReasonName(TurnStopReason reason) {
  switch (reason) {
    case TurnStopReason::kNone:
      return "";
    case TurnStopReason::kTurnDeadline:
      return "turn_deadline";
    case TurnStopReason::kSessionTokenBudget:
      return "session_token_budget";
    case TurnStopReason::kTurnTokens:
      return "turn_tokens";
    case TurnStopReason::kSessionBudget:
      return "session_budget";
    case TurnStopReason::kTurnCost:
      return "turn_cost";
    case TurnStopReason::kMaxToolCalls:
      return "max_tool_calls";
    case TurnStopReason::kRepeatedCalls:
      return "repeated_calls";
    case TurnStopReason::kMaxSteps:
      return "max_steps";
  }
  return "";
}

struct TurnLimits {
  int64_t max_steps = 0;
  int64_t max_tool_calls = 0;
  int64_t max_turn_seconds = 0;
  int64_t max_turn_tokens = 0;
  int64_t session_token_budget = 0;
  double max_turn_cost = 0;
  double session_budget = 0;
};

struct TurnMetrics {
  Usage usage;
  int64_t tool_count = 0;
  double ttt_ms = -1;
  double model_generation_ms = 0;
  int64_t model_generated_tokens = 0;
};

struct TurnStop {
  TurnOutcome outcome = TurnOutcome::kStepLimit;
  TurnStopReason reason = TurnStopReason::kNone;
};

// Whole-turn state: immutable limit snapshots, accounting, and terminal cause.
struct Agent::TurnExecution {
  size_t start = 0;
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point deadline;
  TurnLimits limits;
  TurnMetrics metrics;
  TurnStop stop;
  bool complete = false;
  bool line_open = false;
};

// Mutable loop counters and strategy. ApplyQueuedSteering resets only the
// response-local fields; safety and tool budgets deliberately span the turn.
struct Agent::StepState {
  std::unordered_map<std::string, int64_t> tool_counts;
  std::unordered_map<std::string, std::string> stable_arguments;
  std::unordered_map<std::string, int64_t> rejection_rounds;
  std::string last_call;
  std::string last_single_tool;
  int64_t step = 0;
  int64_t repeated_calls = 0;
  int64_t same_tool_rounds = 0;
  int64_t quiet_activity_id = 0;
  int64_t quiet_activity_polls = 0;
  int64_t consecutive_failed_tools = 0;
  int64_t empty_responses = 0;
  int64_t stop_recoveries = 0;
  int64_t provider_continuations = 0;
  bool failure_advisory_sent = false;
  bool quiet_activity_advisory_sent = false;
  bool markup_recovered = false;
  bool context_overflow_recovery_attempted = false;
  bool detached_records_available = false;
  bool midturn_compaction_enabled = true;
  // Index of a kInternal harness note pushed as the last message of the step,
  // erased once the model has answered it so the nudge never accumulates in
  // history. Only ever the tail at the moment it is set; anything that
  // rewrites history mid-step (compaction) resets it rather than adjusting it,
  // and the erase re-checks the index before trusting it.
  std::optional<size_t> pending_note;
};

}  // namespace uagent

#endif  // UAGENT_SRC_AGENT_TURN_INTERNAL_H_
