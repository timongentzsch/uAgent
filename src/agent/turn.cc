// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/core/time.h"

namespace uagent {

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
  std::string outcome = "step_limit";
};

bool Agent::TurnDeadlineExceeded(TurnState& state,
                                 std::chrono::seconds reserve) {
  if (std::chrono::steady_clock::now() + reserve < state.deadline) return false;
  last_error_ = "turn time limit reached (" +
                std::to_string(state.max_turn_seconds) + "s)";
  state.outcome = "budget_exceeded";
  printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
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
  last_error_ = scope + " cost limit exceeded (" + FmtCost(limit) + ")";
  state.outcome = "budget_exceeded";
  printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
  return true;
}

void Agent::RecordModelResponse(
    ChatResult& response, TurnState& state,
    std::unordered_map<std::string, int64_t>& tool_counts) {
  Usage response_usage;
  response_usage.Add(response.usage);
  if (state.session_budget > 0 && response.usage.is_object() &&
      !response_usage.cost_reported && !cost_warning_shown_) {
    cost_warning_shown_ = true;
    printf(
        "%s· provider does not report cost; dollar budget is not "
        "enforceable%s\n",
        YEL(), RST());
    DebugLog("cost_unavailable", {{"route", ActiveRoute()}});
  }
  // Hidden reasoning predates the first visible stream event.
  double generation_ms = response_usage.reasoning > 0
                             ? response.duration_ms
                             : response.duration_ms - response.first_event_ms;
  if (response.first_event_ms >= 0 && generation_ms > 0 &&
      response_usage.GeneratedTokens() > 0) {
    state.model_generation_ms += generation_ms;
    state.model_generated_tokens += response_usage.GeneratedTokens();
  }
  state.usage.Merge(response_usage);
  MergeSessionUsage(response_usage);
  AddRouteUsage(response_usage);
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
  if (response.usage.is_object()) {
    context_policy_.SetReported(
        JsonValue(response.usage, "prompt_tokens", int64_t{0}) +
        JsonValue(response.usage, "completion_tokens", int64_t{0}));
    ContextUsed();
  }
}

bool Agent::ToolCallsWithinLimits(const std::vector<ToolCall>& calls,
                                  TurnState& state, int64_t max_tool_calls,
                                  std::string& last_call,
                                  int64_t& repeated_calls) {
  if (calls.empty()) return true;
  if (state.tool_count + static_cast<int64_t>(calls.size()) > max_tool_calls) {
    last_error_ =
        "tool call limit reached (" + std::to_string(max_tool_calls) + ")";
    state.outcome = "budget_exceeded";
    printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
    return false;
  }
  bool repeated = false;
  for (const ToolCall& call : calls) {
    std::string signature = call.name + "\n" + call.args;
    repeated_calls = signature == last_call ? repeated_calls + 1 : 1;
    last_call = std::move(signature);
    repeated = repeated || repeated_calls > 3;
  }
  if (!repeated) return true;
  last_error_ = "model repeated the same tool call more than 3 times";
  state.outcome = "budget_exceeded";
  printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
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
      printf("%s· skill %s unavailable: %s%s\n", YEL(),
             TerminalSafe(skill.name).c_str(),
             TerminalSafe(result.output).c_str(), RST());
      continue;
    }
    printf("%s· using skill %s%s\n", DIM(), TerminalSafe(skill.name).c_str(),
           RST());
    selected.push_back(std::move(result.output));
  }
  return selected;
}

void Agent::Turn(const std::string& user_input, json user_content) {
  api_.turn_started = std::chrono::steady_clock::now();
  last_error_.clear();
  checkpoint_turn_complete_ = false;
  ++turn_id_;
  ++revision_;
  ++total_user_turns_;
  if (session_title_.empty()) session_title_ = FirstLine(user_input);
  std::string local_time = LocalStamp();
  ApplyPendingCheckpoint();
  if (!conversation_.Empty()) {
    conversation_.Set(0, SysMsg(), MessageKind::kSystem);
  }
  DebugLog("turn_start",
           {{"turn", turn_id_},
            {"local_time", local_time},
            {"input", user_input},
            {"attachments", user_content.is_array() && !user_content.empty()
                                ? user_content.size() - 1
                                : 0},
            {"messages", conversation_.Size()},
            {"context_tokens", ContextUsed()}});
  bool attachment = !user_content.is_null();
  checkpoint_hint_active_ = false;
  std::vector<std::string> explicit_skills = ExplicitSkillContext(user_input);
  size_t skill_bytes = 0;
  for (const std::string& skill : explicit_skills) {
    skill_bytes = SaturatingAdd(skill_bytes, skill.size());
  }
  std::string checkpoint_hint = PrepareContext(SaturatingAdd(
      attachment ? JsonEstimatedBytes(user_content) : user_input.size(),
      skill_bytes));
  if (SteeringState().Requested() && SteeringState().QueuedCount() == 0) {
    DebugLog("turn_end", {{"turn", turn_id_},
                          {"outcome", "steered_during_compaction"},
                          {"steps", 0}});
    api_.turn_started = {};
    return;
  }
  EnsureRuntimeContext();
  TurnState state;
  state.start = conversation_.Size();  // user message and prune_* start
  for (std::string& skill : explicit_skills) {
    conversation_.Push(
        HarnessMessage("[explicit skill instructions; user selected]\n" +
                       std::move(skill)),
        MessageKind::kInternal);
  }
  conversation_.Push(
      {{"role", "user"},
       {"content", attachment ? std::move(user_content) : json(user_input)}},
      attachment ? MessageKind::kAttachment : MessageKind::kUser);
  if (!checkpoint_hint.empty()) {
    conversation_.Push(HarnessMessage(std::move(checkpoint_hint)),
                       MessageKind::kInternal);
    checkpoint_hint_active_ = true;
    context_policy_.HintIssued(turn_id_);
  }
  turn_search_trace_.Reset();
  std::unordered_map<std::string, int64_t> tool_counts;
  std::unordered_map<std::string, std::string> stable_arguments;
  state.max_steps = api_.config.max_steps;
  state.max_turn_seconds = api_.config.max_turn_seconds;
  state.max_turn_cost = api_.config.max_turn_cost;
  state.session_budget = api_.config.session_budget;
  state.deadline = DeadlineAfter(state.started, state.max_turn_seconds);
  active_deadline_ = state.deadline;
  std::string last_call;
  int64_t repeated_calls = 0;
  int64_t consecutive_failed_tools = 0;
  bool failure_advisory_sent = false;
  bool empty_response_recovered = false;
  std::optional<size_t> empty_recovery_message;
  std::optional<size_t> failure_advisory_message;
  bool detached_records_available = !DetachedRecords().empty();
  bool midturn_compaction_enabled = true;

  auto apply_queued_steering = [&] {
    std::vector<std::string> queued = SteeringState().TakeQueued();
    if (queued.empty()) return false;
    SteeringState().Take();
    for (std::string& input : queued) {
      for (std::string& skill : ExplicitSkillContext(input)) {
        conversation_.Push(
            HarnessMessage("[explicit skill instructions; user selected]\n" +
                           std::move(skill)),
            MessageKind::kInternal);
      }
      conversation_.Push({{"role", "user"}, {"content", std::move(input)}},
                         MessageKind::kUser);
    }
    last_call.clear();
    repeated_calls = 0;
    consecutive_failed_tools = 0;
    stable_arguments.clear();
    failure_advisory_sent = false;
    empty_response_recovered = false;
    DebugLog("steering_applied",
             {{"turn", turn_id_}, {"messages", queued.size()}});
    return true;
  };

  int64_t step = 0;
  for (; step < state.max_steps; ++step) {
    apply_queued_steering();
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
        .checkpoint_hint = checkpoint_hint_active_,
        .detached_terminal = processes_.PendingCount() > 0 ||
                             processes_.DetachedCount() > 0 ||
                             detached_records_available,
    };
    json available_schemas =
        AvailableToolSchemas(tools_, schemas_, tool_counts, availability);
    if (step > 0 && midturn_compaction_enabled &&
        MaybeCompactDuringTurn(available_schemas, user_input, state.usage,
                               state.start) != MidturnCompact::kNotNeeded) {
      midturn_compaction_enabled = false;
      --step;
      continue;
    }
    ChatResult r = Chat("turn", step, available_schemas);
    if (failure_advisory_message) {
      conversation_.Erase(*failure_advisory_message,
                          *failure_advisory_message + 1);
      failure_advisory_message.reset();
    }
    if (empty_recovery_message) {
      conversation_.Erase(*empty_recovery_message, *empty_recovery_message + 1);
      empty_recovery_message.reset();
    }

    if (r.interrupted) {
      state.line_open = false;
      state.outcome = "interrupted";
      last_error_ = state.outcome;
      printf("\n%s· interrupted%s\n", YEL(), RST());
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
      if (DegradeAndRetry(r)) {
        --step;
        continue;
      }
      state.outcome = "error";
      last_error_ = r.error;
      printf("%s%s%s\n", RED(), TerminalSafe(r.error).c_str(), RST());
      break;
    }

    RecordModelResponse(r, state, tool_counts);
    if (TurnCostExceeded(state)) break;
    std::vector<ToolCall> calls = std::move(r.tool_calls);
    bool text_mode =
        calls.empty() && !(calls = ParseTextToolCalls(r.content)).empty();

    if (!ToolCallsWithinLimits(calls, state, api_.config.max_tool_calls,
                               last_call, repeated_calls)) {
      break;
    }

    if (calls.empty() && r.content.empty()) {
      if (!empty_response_recovered && state.tool_count > 0) {
        empty_response_recovered = true;
        conversation_.Push(
            HarnessMessage("[empty model response] Return the final answer "
                           "from existing results. Do not repeat completed "
                           "work."),
            MessageKind::kInternal);
        empty_recovery_message = conversation_.Size() - 1;
        DebugLog("empty_response_recovery",
                 {{"turn", turn_id_}, {"step", step}});
        printf("%s· recovering empty response%s\n", DIM(), RST());
        continue;
      }
      state.outcome = "error";
      last_error_ = "model returned an empty response";
      printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
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
      api_.PreserveAssistantReasoning(amsg, r);
    }
    conversation_.Push(std::move(amsg), MessageKind::kAssistant);

    if (calls.empty()) {
      state.ttt_ms = r.first_event_ms;
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
      failure_advisory_message = conversation_.Size() - 1;
      DebugLog("tool_failure_advisory",
               {{"turn", turn_id_},
                {"step", step},
                {"consecutive_failures", consecutive_failed_tools}});
    }
    bool foreground_interrupted = SteeringState().Requested() || cancelled;
    bool steering_applied = apply_queued_steering();
    if (cancelled) BgCancelTasks(processes_);
    if (checkpoint_turn_complete_) {
      state.complete = true;
      state.outcome = "checkpoint_prepared";
      break;
    }
    if (foreground_interrupted) {
      if (steering_applied) continue;
      state.outcome = "interrupted";
      if (cancelled) printf("%s· interrupted%s\n", YEL(), RST());
      break;
    }
    // Do not start a network request with only curl's one-second granularity
    // left after tools. Report the owning turn budget instead of a misleading
    // transport timeout that cannot possibly be retried.
    if (TurnDeadlineExceeded(state, std::chrono::seconds(1))) break;
  }
  FinishTurn(state, step);
}

void Agent::FinishTurn(TurnState& state, int64_t step) {
  if (step >= state.max_steps) {
    last_error_ =
        "step limit (" + std::to_string(state.max_steps) + ") reached";
    std::cout << RED() << "step limit (" << state.max_steps
              << ") reached — stopping this turn" << RST() << '\n';
  }
  PruneAttachments(state.start);
  if (!checkpoint_turn_complete_) ArchiveTurnTrace(state.start);
  PruneOldToolResults();
  if (pending_checkpoint_.is_object() &&
      JsonValue(pending_checkpoint_, "turn", int64_t{-1}) == turn_id_) {
    if (state.complete && !processes_.PendingCount()) {
      pending_checkpoint_["ready"] = true;
      DebugLog(
          "checkpoint_ready",
          {{"turn", turn_id_},
           {"state_chars",
            JsonValue(pending_checkpoint_, "state", std::string()).size()}});
    } else {
      InvalidatePendingCheckpoint(state.complete
                                      ? "background work is still active"
                                      : "checkpoint turn did not complete");
    }
  }

  MergeSideUsage(state.usage);

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              state.started)
                    .count();
  double tokens_per_second =
      state.model_generation_ms > 0
          ? state.model_generated_tokens * 1000.0 / state.model_generation_ms
          : 0;
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
    stats << " · first " << std::fixed << std::setprecision(2)
          << state.ttt_ms / 1000.0 << 's';
  }
  stats << " · " << std::fixed << std::setprecision(1) << secs << 's';
  std::string stats_line = stats.str();
  std::ostringstream footer;
  footer << (state.line_open ? "\n" : "") << RST() << BLUE() << stats_line
         << RST() << '\n';
  std::cout << footer.str();
  DebugLog("turn_end",
           {{"turn", turn_id_},
            {"outcome", state.outcome},
            {"steps", step >= state.max_steps ? state.max_steps : step + 1},
            {"tool_calls", state.tool_count},
            {"duration_ms", secs * 1000},
            {"ttt_ms", state.ttt_ms},
            {"tokens_per_second", tokens_per_second},
            {"generation_ms", state.model_generation_ms},
            {"generated_tokens", state.model_generated_tokens},
            {"usage", UsageJson(state.usage)},
            {"session_usage", UsageJson(session_usage_)},
            {"messages", conversation_.Size()},
            {"context_tokens", ContextUsed()}});
  checkpoint_hint_active_ = false;
  active_deadline_ = std::chrono::steady_clock::time_point::max();
  api_.turn_started = {};
}

}  // namespace uagent
