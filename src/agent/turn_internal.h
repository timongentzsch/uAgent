// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_AGENT_TURN_INTERNAL_H_
#define UAGENT_SRC_AGENT_TURN_INTERNAL_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "include/agent.h"
#include "include/core/env.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
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

// A turn's limits are the configured budgets as they stood when it started,
// so this is the config's own budget block rather than a second declaration of
// it. Named separately because a turn reads limits, never settings.
using TurnLimits = TurnBudgets;

struct TurnMetrics {
  bool usage_reported = false;
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
  int64_t model_calls_before = 0;
  bool complete = false;
  bool line_open = false;
};

// A mistake the model makes in a response or a round of calls. It never ends
// a turn outright: each is counted, the model is told what to do instead, and
// only a model that keeps ignoring that is stopped.
enum class Fault {
  kEmpty,
  kMarkup,
  kNameless,
  kCutOff,
  kRepeat,
  kQuietPoll,
  kFailedTools,
  kCount
};

// `{n}` is the strike and `{x}` what the fault is about.
struct FaultRule {
  const char* name;
  int64_t stop_after;          // strikes that end the turn; 0 never does
  TurnStopReason stop_reason;  // kNone ends it as an error, else as a budget
  const char* stopped;         // what the person is told then
  struct {
    int64_t at;        // 0: every strike
    const char* note;  // what the model is told
  } advice[2];
};

inline constexpr char kStoppedBeforeCompletion[] =
    "model response stopped before completion ({x})";
#define UAGENT_WAIT_INSTEAD \
  "use a bounded wait operation when you are observing ongoing work."
inline constexpr FaultRule kFaultRules[] = {
    // The first replay goes out unchanged: only a repeat is evidence that
    // the model needs steering rather than another attempt.
    {"empty_response",
     3,
     TurnStopReason::kNone,
     "model returned an empty response",
     {{2, "[empty model response] {x}"}}},
    // Text that imitates a tool protocol but parses as nothing.
    {"tool_markup",
     2,
     TurnStopReason::kNone,
     "model repeatedly returned invalid tool markup",
     {{0,
       "[invalid model tool markup] The attempted call was not executed. "
       "Return prose using existing results; do not imitate a tool "
       "protocol."}}},
    // A call that names no function: nothing can answer it, so it is dropped.
    {"nameless_call",
     3,
     TurnStopReason::kNone,
     "model kept sending tool calls without a function name",
     {{0,
       "[invalid model tool call] A tool call arrived without a function "
       "name and was not run. Send it again as one complete call."}}},
    {"partial_response",
     2,
     TurnStopReason::kNone,
     kStoppedBeforeCompletion,
     {{0,
       "[partial model response: {x}] Continue exactly where the response "
       "stopped. Do not repeat completed content or work."}}},
    // Valid repetition is recoverable; the ceiling only catches a model that
    // ignores both instructions.
    {"repeated_call",
     kRepeatedCallStopAfter,
     TurnStopReason::kRepeatedCalls,
     "model repeated the same tool call {n} times after two recovery "
     "instructions",
     {{kRepeatedCallAdviseAfter,
       "[repeated tool advisory] The same tool and arguments have already "
       "run {n} consecutive times. Reassess whether another identical result "
       "can add evidence. Use the current result, change the request, "
       "or " UAGENT_WAIT_INSTEAD},
      {kRepeatedCallDirectAfter,
       "[repeated tool advisory] The same tool and arguments have already "
       "run {n} consecutive times. Do not issue that unchanged call again. "
       "Use its existing result, change the arguments or strategy, "
       "or " UAGENT_WAIT_INSTEAD}}},
    // A slow activity answers "(no new output)" honestly, so quiet polls
    // steer rather than end the turn that owns the work.
    {"quiet_poll",
     kActivityPollStopAfter,
     TurnStopReason::kNone,
     "activity {x} is still running, but the model polled it {n} times "
     "without new output and without waiting on it",
     {{kActivityPollAdviseAfter,
       "[activity poll advisory] Activity {x} is still running and has "
       "returned no new output {n} times. Do not poll it again immediately. "
       "If completion blocks the next step, issue one bounded activity call "
       "with operation=wait, mode=any, and wait_ms; otherwise continue "
       "independent work."},
      {kActivityPollDirectAfter,
       "[activity poll advisory] Activity {x} is still running and has "
       "returned no new output {n} times. Stop polling it: this turn ends in "
       "an error if you keep polling without waiting. Either issue one "
       "activity call with operation=wait, mode=any and wait_ms, or read the "
       "output it writes elsewhere, or do independent work and revisit it "
       "later."}}},
    {"failed_tools",
     0,
     TurnStopReason::kNone,
     "",
     {{kFailedToolAdviseAfter,
       "[tool failure advisory] {n} consecutive tool calls failed. Reassess "
       "the shared premise or execution environment before trying another "
       "variant; use existing evidence or a different approach when "
       "possible."}}},
};
#undef UAGENT_WAIT_INSTEAD

// A rule's text with its strike and subject filled in.
inline std::string FaultText(std::string text, int64_t strike,
                             const std::string& about) {
  for (auto [slot, value] :
       {std::pair<const char*, std::string>{"{n}", std::to_string(strike)},
        {"{x}", about}}) {
    if (size_t at = text.find(slot); at != std::string::npos) {
      text.replace(at, 3, value);
    }
  }
  return text;
}

// Mutable loop counters and strategy. Safety and tool budgets span the turn;
// only `recovery` starts over when steering changes the question.
struct Agent::StepState {
  // How this line of responses has been going: repetitions, failures and the
  // advisories already sent about them.
  struct Recovery {
    std::unordered_map<std::string, int64_t> rejection_rounds;
    std::string last_call;
    std::string last_single_tool;
    int64_t same_tool_rounds = 0;
    int64_t quiet_activity_id = 0;
    std::array<int64_t, static_cast<size_t>(Fault::kCount)> strikes{};
  };
  Recovery recovery;
  std::unordered_map<std::string, int64_t> tool_counts;
  int64_t step = 0;
  int64_t provider_continuations = 0;
  bool context_overflow_recovery_attempted = false;
  bool detached_records_available = false;
  bool midturn_compaction_enabled = true;
  // What the harness has to tell the model about this step. It goes out as
  // the last message of the next request and is erased once answered, so a
  // nudge never accumulates in history.
  std::string note;
};

}  // namespace uagent

#endif  // UAGENT_SRC_AGENT_TURN_INTERNAL_H_
