// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
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

namespace uagent {

namespace {

bool GenericSessionTitle(std::string title) {
  title = AsciiLower(Trim(title));
  return title == "hi" || title == "hello" || title == "hey" || title == "test";
}

}  // namespace

struct Agent::TurnState {
  size_t start = 0;
  Usage usage;
  int64_t tool_count = 0;
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point deadline;
  int64_t max_steps = 0;
  int64_t max_turn_seconds = 0;
  double max_turn_cost = 0;
  double session_budget = 0;
  bool complete = false;
  bool line_open = false;
  double ttt_ms = -1;
  double model_generation_ms = 0;
  int64_t model_generated_tokens = 0;
  std::string last_single_tool;
  int64_t same_tool_rounds = 0;
  std::string outcome = "step_limit";
};

// Every bound the turn enforces ends the same way: record why, mark the turn,
// and say so in red.
void Agent::FailBudget(TurnState& state, std::string message) {
  last_error_ = std::move(message);
  state.outcome = "budget_exceeded";
  Emit(NoticeEvent(PresentationStatus::kFailed, last_error_));
}

bool Agent::TurnDeadlineExceeded(TurnState& state,
                                 std::chrono::seconds reserve) {
  if (std::chrono::steady_clock::now() + reserve < state.deadline) return false;
  FailBudget(state, "turn time limit reached (" +
                        std::to_string(state.max_turn_seconds) + "s)");
  return true;
}

bool Agent::TurnCostExceeded(TurnState& state) {
  double limit = state.max_turn_cost;
  double spent = state.usage.cost;
  std::string scope = "turn";
  if (state.session_budget > 0 && session_usage_.cost > state.session_budget) {
    limit = state.session_budget;
    spent = session_usage_.cost;
    scope = "session";
  }
  if (limit <= 0 || spent <= limit) return false;
  FailBudget(state, scope + " cost limit exceeded (" + FmtCost(limit) + ")");
  return true;
}

void Agent::RecordModelResponse(
    ChatResult& response, TurnState& state,
    std::unordered_map<std::string, int64_t>& tool_counts) {
  Usage response_usage = AccountModelUsage(response.usage);
  if (state.ttt_ms < 0 && response.first_event_ms >= 0) {
    state.ttt_ms = response.first_event_ms;
  }
  if (state.session_budget > 0 && response.usage.is_object() &&
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
    state.model_generation_ms += response.duration_ms;
    state.model_generated_tokens += response_usage.GeneratedTokens();
  }
  state.usage.Merge(response_usage);
  tool_counts["web_search"] += response_usage.web_searches;
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
                                  TurnState& state, int64_t max_tool_calls,
                                  std::string& last_call,
                                  int64_t& repeated_calls) {
  if (calls.empty()) return true;
  if (max_tool_calls > 0 &&
      state.tool_count + static_cast<int64_t>(calls.size()) > max_tool_calls) {
    FailBudget(state, "tool call limit reached (" +
                          std::to_string(max_tool_calls) + ")");
    return false;
  }
  auto blocking_wait = [&](const ToolCall& call) {
    const Tool* tool = FindTool(tools_, call.name);
    if (!tool || tool->blocking_wait_default_ms < 0) return false;
    json arguments = json::parse(call.args, nullptr, false);
    return JsonValue(arguments, "wait_ms", tool->blocking_wait_default_ms) > 0;
  };
  bool repeated = false;
  for (const ToolCall& call : calls) {
    if (blocking_wait(call)) {
      last_call.clear();
      repeated_calls = 0;
      continue;
    }
    std::string signature = call.name + "\n" + call.args;
    repeated_calls = signature == last_call ? repeated_calls + 1 : 1;
    last_call = std::move(signature);
    repeated = repeated || repeated_calls > 3;
  }
  if (!repeated) return true;
  FailBudget(state, "model repeated the same tool call more than 3 times");
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
  auto push_skill = [&](std::string skill) {
    conversation_.Push(
        HarnessMessage("[explicit skill instructions; user selected]\n" +
                       std::move(skill)),
        MessageKind::kInternal);
  };
  TurnState state;
  state.start = conversation_.Size();  // user message and prune_* start
  for (std::string& skill : explicit_skills) push_skill(std::move(skill));
  conversation_.Push(
      {{"role", "user"},
       {"content", attachment ? std::move(user_content) : json(user_input)}},
      attachment ? MessageKind::kAttachment : MessageKind::kUser);
  turn_search_trace_.Reset();
  std::unordered_map<std::string, int64_t> tool_counts;
  std::unordered_map<std::string, std::string> stable_arguments;
  state.max_steps = api_.config.max_steps;
  state.max_turn_seconds = api_.config.max_turn_seconds;
  state.max_turn_cost = api_.config.max_turn_cost;
  state.session_budget = api_.config.session_budget;
  state.deadline = state.max_turn_seconds > 0
                       ? DeadlineAfter(state.started, state.max_turn_seconds)
                       : std::chrono::steady_clock::time_point::max();
  active_deadline_ = state.deadline;
  std::string last_call;
  int64_t repeated_calls = 0;
  int64_t consecutive_failed_tools = 0;
  bool failure_advisory_sent = false;
  bool markup_recovered = false;
  // A completion with no answer and no call carries nothing to react to, so
  // the first one is replayed unchanged, a repeat earns a guiding note, and
  // only a third ends the turn: a barren provider response must not cost the
  // work this turn has already done.
  constexpr int64_t kEmptyResponseAttempts = 3;
  int64_t empty_responses = 0;
  bool context_overflow_recovery_attempted = false;
  // A harness note that guides exactly the next model call and is retracted
  // once it has been sent. At most one is ever live.
  std::optional<size_t> pending_note;
  bool detached_records_available = !DetachedRecords().empty();
  bool midturn_compaction_enabled = true;

  auto apply_queued_steering = [&] {
    std::vector<std::string> queued = SteeringState().TakeQueued();
    if (queued.empty()) return false;
    SteeringState().Take();
    for (std::string& input : queued) {
      for (std::string& skill : ExplicitSkillContext(input)) {
        push_skill(std::move(skill));
      }
      conversation_.Push({{"role", "user"}, {"content", std::move(input)}},
                         MessageKind::kUser);
    }
    last_call.clear();
    repeated_calls = 0;
    consecutive_failed_tools = 0;
    state.last_single_tool.clear();
    state.same_tool_rounds = 0;
    stable_arguments.clear();
    failure_advisory_sent = false;
    markup_recovered = false;
    empty_responses = 0;
    DebugLog("steering_applied",
             {{"turn", turn_id_}, {"messages", queued.size()}});
    return true;
  };

  int64_t step = 0;
  for (; state.max_steps <= 0 || step < state.max_steps; ++step) {
    apply_queued_steering();
    RefreshSystemMessage();
    if (SteeringState().Requested()) {
      state.outcome = "interrupted";
      last_error_ = state.outcome;
      break;
    }
    if (TurnDeadlineExceeded(state)) break;
    if (refresh_tools_ && refresh_tools_(state.deadline)) RebuildToolSchemas();
    if (TurnDeadlineExceeded(state)) break;
    DrainBackground();
    MergeSideUsage(state.usage);
    if (TurnCostExceeded(state)) break;
    ToolAvailability availability{
        .detached_terminal = processes_.PendingCount() > 0 ||
                             processes_.DetachedCount() > 0 ||
                             detached_records_available,
    };
    json available_schemas =
        AvailableToolSchemas(tools_, schemas_, tool_counts, availability);
    if (step > 0 && midturn_compaction_enabled) {
      MidturnCompact compacted =
          MaybeCompactDuringTurn(available_schemas, state.usage, state.start);
      if (compacted != MidturnCompact::kNotNeeded) {
        midturn_compaction_enabled = false;
        // A successful compaction rebuilt the history, so a recorded note
        // index no longer refers to its note; a failed one left history
        // exactly as it was, and the note still has to be retracted.
        if (compacted == MidturnCompact::kSucceeded) pending_note.reset();
        --step;
        continue;
      }
    }
    ChatResult r = Chat("turn", step, available_schemas);
    if (pending_note) {
      conversation_.Erase(*pending_note, *pending_note + 1);
      pending_note.reset();
    }

    if (r.interrupted) {
      state.line_open = false;
      state.outcome = "interrupted";
      last_error_ = state.outcome;
      printf("\n");
      Emit(NoticeEvent(PresentationStatus::kWarned, "· interrupted"));
      conversation_.Push(
          HarnessMessage("(response interrupted; partial output was "
                         "discarded)"),
          MessageKind::kInternal);
      if (apply_queued_steering()) continue;
      break;
    }
    if (!r.error.empty()) {
      state.line_open = false;
      if (TurnDeadlineExceeded(state)) break;
      if (!context_overflow_recovery_attempted && SafeContextRecovery(r) &&
          !attachment) {
        context_overflow_recovery_attempted = true;
        int64_t rejected_tokens = EstimatedTokens(
            RequestContextBytes(JsonEstimatedBytes(available_schemas)));
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
        DebugLog("context_overflow_recovery",
                 {{"turn", turn_id_},
                  {"step", step},
                  {"rejected_tokens", rejected_tokens},
                  {"learned_context", api_.ctx_window},
                  {"messages", conversation_.Size()}});
        Emit(NoticeEvent(PresentationStatus::kNeutral,
                         "· provider context limit reached — compacting once"));
        midturn_compaction_enabled = false;
        if (Compact(true, &state.usage)) {
          state.start = conversation_.Size();
          --step;
          continue;
        }
        state.outcome = "error";
        last_error_ = r.error;
        break;
      }
      if (r.remote_error_kind == RemoteErrorKind::kContextLengthExceeded) {
        DebugLog("context_overflow_recovery_skipped",
                 {{"turn", turn_id_},
                  {"step", step},
                  {"already_attempted", context_overflow_recovery_attempted},
                  {"attachment", attachment},
                  {"semantic_progress", r.semantic_progress}});
      }
      if (DegradeAndRetry(r)) {
        --step;
        continue;
      }
      state.outcome = "error";
      last_error_ = r.error;
      Emit(NoticeEvent(PresentationStatus::kFailed, r.error));
      break;
    }

    RecordModelResponse(r, state, tool_counts);
    if (TurnCostExceeded(state)) break;
    std::vector<ToolCall> calls = std::move(r.tool_calls);
    std::vector<ToolCall> text_calls;
    if (calls.empty()) text_calls = ParseTextToolCalls(r.content);
    bool text_mode = !api_.capabilities.native_tools && !text_calls.empty();
    if (text_mode) calls = std::move(text_calls);

    if (calls.empty() &&
        (!text_calls.empty() || ContainsForeignToolCallMarkup(r.content) ||
         r.suppressed)) {
      if (!markup_recovered) {
        markup_recovered = true;
        conversation_.Push(
            HarnessMessage("[invalid model tool markup] The attempted call was "
                           "not executed. Return prose using existing results; "
                           "do not imitate a tool protocol."),
            MessageKind::kInternal);
        pending_note = conversation_.Size() - 1;
        DebugLog("foreign_tool_markup_recovery",
                 {{"turn", turn_id_}, {"step", step}});
        continue;
      }
      state.outcome = "error";
      last_error_ = "model repeatedly returned invalid tool markup";
      break;
    }

    if (!ToolCallsWithinLimits(calls, state, api_.config.max_tool_calls,
                               last_call, repeated_calls)) {
      break;
    }
    if (calls.size() == 1) {
      state.same_tool_rounds = calls[0].name == state.last_single_tool
                                   ? state.same_tool_rounds + 1
                                   : 1;
      state.last_single_tool = calls[0].name;
      if (state.same_tool_rounds == 8) {
        DebugLog("repeated_tool_rounds", {{"turn", turn_id_},
                                          {"step", step},
                                          {"tool", calls[0].name},
                                          {"rounds", state.same_tool_rounds}});
      }
    } else {
      state.last_single_tool.clear();
      state.same_tool_rounds = 0;
    }

    if (calls.empty() && r.content.empty()) {
      if (++empty_responses < kEmptyResponseAttempts) {
        // The first replay goes out unchanged: only a repeat is evidence that
        // the model needs steering rather than another attempt.
        if (empty_responses > 1) {
          conversation_.Push(
              HarnessMessage(state.tool_count > 0
                                 ? "[empty model response] Return the final "
                                   "answer from existing results. Do not "
                                   "repeat completed work."
                                 : "[empty model response] The previous reply "
                                   "arrived empty. Answer the request "
                                   "directly."),
              MessageKind::kInternal);
          pending_note = conversation_.Size() - 1;
        }
        DebugLog("empty_response_recovery",
                 {{"turn", turn_id_},
                  {"step", step},
                  {"attempt", empty_responses},
                  {"guided", empty_responses > 1},
                  {"finish_reason", r.finish_reason}});
        Emit(NoticeEvent(PresentationStatus::kNeutral,
                         "· recovering empty response"));
        continue;
      }
      state.outcome = "error";
      last_error_ = "model returned an empty response";
      Emit(NoticeEvent(PresentationStatus::kFailed, last_error_));
      break;
    }

    json amsg = {{"role", "assistant"}, {"content", r.content}};
    if (!calls.empty() && !text_mode) {
      json tcs = json::array();
      for (const ToolCall& c : calls) {
        tcs.push_back(
            {{"id", c.id},
             {"type", "function"},
             {"function", {{"name", c.name}, {"arguments", c.args}}}});
      }
      amsg["tool_calls"] = std::move(tcs);
      // Tool-only turns carry no prose; store null so strict backends
      // (e.g. Anthropic) don't reject an empty text block on replay.
      if (r.content.empty()) amsg["content"] = nullptr;
    }
    // Preserve the replay fields the active route actually emitted while any
    // tool protocol continues; completed prose does not burden later turns.
    if (!calls.empty()) api_.PreserveAssistantReasoning(amsg, r);
    conversation_.Push(std::move(amsg), MessageKind::kAssistant);

    if (calls.empty()) {
      // content that looked like a tool call was held back from the
      // stream; if it didn't parse into one, it's prose — show it now
      if (r.suppressed) {
        MdPrint(r.content);
        printf("\n");
      }
      if (apply_queued_steering()) continue;
      if (SteeringState().Requested()) {
        state.outcome = "interrupted";
        last_error_ = state.outcome;
        break;
      }
      state.complete = true;
      state.outcome = "complete";
      break;  // plain prose -> turn is done
    }
    if (state.line_open) printf("\n");
    bool cancelled = RunCalls(calls, text_mode, state.tool_count, tool_counts,
                              stable_arguments, step, state.deadline,
                              consecutive_failed_tools);
    state.line_open = false;
    if (!failure_advisory_sent && consecutive_failed_tools >= 3) {
      failure_advisory_sent = true;
      conversation_.Push(
          HarnessMessage("[tool failure advisory] Three consecutive tool "
                         "calls failed. Reassess the shared premise or "
                         "execution environment before trying another "
                         "variant; use existing evidence or a different "
                         "approach when possible."),
          MessageKind::kInternal);
      pending_note = conversation_.Size() - 1;
      DebugLog("tool_failure_advisory",
               {{"turn", turn_id_},
                {"step", step},
                {"consecutive_failures", consecutive_failed_tools}});
    }
    bool foreground_interrupted = SteeringState().Requested() || cancelled;
    bool steering_applied = apply_queued_steering();
    if (cancelled) BgCancelSubagents(processes_);
    if (foreground_interrupted) {
      if (steering_applied) continue;
      state.outcome = "interrupted";
      if (cancelled) {
        Emit(NoticeEvent(PresentationStatus::kWarned, "· interrupted"));
      }
      break;
    }
    // Do not start a network request with only curl's one-second granularity
    // left after tools. Report the owning turn budget instead of a misleading
    // transport timeout that cannot possibly be retried.
    if (TurnDeadlineExceeded(state, std::chrono::seconds(1))) break;
  }
  FinishTurn(state, step);
}

// The one-line accounting footer printed after every turn.
std::string Agent::TurnStatsLine(const TurnState& state, double seconds,
                                 double tokens_per_second) {
  std::ostringstream stats;
  stats << FmtCount(state.usage.input) << " in";
  if (state.usage.cache_read) {
    stats << " (+" << FmtCount(state.usage.cache_read) << " cached)";
  }
  if (state.usage.cache_write) {
    stats << " (+" << FmtCount(state.usage.cache_write) << " cache write)";
  }
  stats << " · " << FmtCount(state.usage.output) << " out";
  if (state.usage.reasoning) {
    stats << ' ' << ITAL() << "(+" << FmtCount(state.usage.reasoning)
          << " reasoning)" << ItalOff();
  }
  if (state.usage.cost > 0) stats << " · " << FmtCost(state.usage.cost);
  if (state.usage.web_searches) {
    stats << " · " << state.usage.web_searches << " search"
          << (state.usage.web_searches == 1 ? "" : "es");
  }
  if (state.tool_count) {
    stats << " · " << state.tool_count << " tool"
          << (state.tool_count == 1 ? "" : "s");
  }
  if (tokens_per_second > 0) {
    stats << " · " << std::fixed << std::setprecision(1) << tokens_per_second
          << " tok/s";
  }
  if (state.ttt_ms >= 0) {
    stats << " · first " << FmtDuration(state.ttt_ms / 1000.0);
  }
  stats << " · " << FmtDuration(seconds);
  return stats.str();
}

void Agent::FinishTurn(TurnState& state, int64_t step) {
  if (state.max_steps > 0 && step >= state.max_steps) {
    last_error_ =
        "step limit (" + std::to_string(state.max_steps) + ") reached";
    Emit(NoticeEvent(PresentationStatus::kFailed,
                     last_error_ + " — stopping this turn"));
  }
  PruneAttachments(state.start);
  ArchiveTurnTrace(state.start);
  PruneOldToolResults();

  MergeSideUsage(state.usage);

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              state.started)
                    .count();
  double tokens_per_second =
      state.model_generation_ms > 0
          ? state.model_generated_tokens * 1000.0 / state.model_generation_ms
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
              {"outcome", state.outcome},
              {"steps", state.max_steps > 0 && step >= state.max_steps
                            ? state.max_steps
                            : step + 1},
              {"tool_calls", state.tool_count},
              {"duration_ms", secs * 1000},
              {"ttt_ms", state.ttt_ms},
              {"tokens_per_second", tokens_per_second},
              {"generation_ms", state.model_generation_ms},
              {"generated_tokens", state.model_generated_tokens},
              {"usage", UsageJson(state.usage)},
              {"session_usage", UsageJson(session_usage_)},
              {"messages", conversation_.Size()},
              {"context_tokens", ContextUsed()}}});
  active_deadline_ = std::chrono::steady_clock::time_point::max();
  api_.turn_started = {};
}

}  // namespace uagent
