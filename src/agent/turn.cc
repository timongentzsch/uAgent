// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent.h"

namespace uagent {

void Agent::Turn(const std::string& user_input, json user_content) {
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
  size_t pending_chars = user_content.is_null()
                             ? user_input.size()
                             : JsonEstimatedBytes(user_content);
  checkpoint_hint_active_ = false;
  std::string checkpoint_hint = PrepareContext(pending_chars);
  if (SteeringState().Requested()) {
    DebugLog("turn_end", {{"turn", turn_id_},
                          {"outcome", "steered_during_compaction"},
                          {"steps", 0}});
    return;
  }
  EnsureRuntimeContext();
  size_t turn_start =
      conversation_.Size();  // the user message; prune_* index from it
  MessageKind user_kind =
      user_content.is_null() ? MessageKind::kUser : MessageKind::kAttachment;
  conversation_.Push(
      {{"role", "user"},
       {"content",
        user_content.is_null() ? json(user_input) : std::move(user_content)}},
      user_kind);
  if (!checkpoint_hint.empty()) {
    conversation_.Push(HarnessMessage(std::move(checkpoint_hint)),
                       MessageKind::kInternal);
    checkpoint_hint_active_ = true;
    context_policy_.HintIssued(turn_id_);
  }
  Usage usage;
  turn_search_trace_.Reset();
  int64_t tool_count = 0;
  std::unordered_map<std::string, int64_t> tool_counts;
  auto t0 = std::chrono::steady_clock::now();
  int64_t max_steps = api_.config.max_steps;
  int64_t max_tool_calls = api_.config.max_tool_calls;
  int64_t max_turn_seconds = api_.config.max_turn_seconds;
  double max_turn_cost = api_.config.max_turn_cost;
  double session_budget = api_.config.session_budget;
  auto deadline = t0 + std::chrono::seconds(max_turn_seconds);
  active_deadline_ = deadline;
  std::string last_call;
  int64_t repeated_calls = 0;
  int64_t consecutive_failed_tools = 0;
  bool failure_advisory_sent = false;
  bool complete = false;
  bool line_open = false;
  bool empty_response_recovered = false;
  std::optional<size_t> empty_recovery_message;
  std::optional<size_t> failure_advisory_message;
  bool detached_records_available = !DetachedRecords().empty();
  double ttt_ms = -1;
  double model_response_ms = 0;
  int64_t model_output_tokens = 0;
  std::string outcome = "step_limit";
  bool midturn_compaction_enabled = true;
  auto cost_exceeded = [&] {
    double limit = max_turn_cost;
    double spent = usage.cost;
    std::string scope = "turn";
    if (session_budget > 0 && session_usage_.cost > session_budget) {
      limit = session_budget;
      spent = session_usage_.cost;
      scope = "session";
    }
    if (limit <= 0 || spent <= limit) return false;
    last_error_ = scope + " cost limit exceeded (" + FmtCost(limit) + ")";
    outcome = "budget_exceeded";
    printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
    return true;
  };

  int64_t step = 0;
  for (; step < max_steps; ++step) {
    if (std::chrono::steady_clock::now() >= deadline) {
      last_error_ =
          "turn time limit reached (" + std::to_string(max_turn_seconds) + "s)";
      outcome = "budget_exceeded";
      printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
      break;
    }
    if (refresh_tools_ && refresh_tools_(deadline)) RebuildToolSchemas();
    if (std::chrono::steady_clock::now() >= deadline) {
      last_error_ =
          "turn time limit reached (" + std::to_string(max_turn_seconds) + "s)";
      outcome = "budget_exceeded";
      printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
      break;
    }
    DrainBackground();
    MergeSideUsage(usage);
    if (cost_exceeded()) break;
    ToolAvailability availability{
        .checkpoint_hint = checkpoint_hint_active_,
        .detached_terminal =
            processes_.DetachedCount() > 0 || detached_records_available,
    };
    json available_schemas =
        AvailableToolSchemas(tools_, schemas_, tool_counts, availability);
    MidturnCompact compact = MidturnCompact::kNotNeeded;
    if (step > 0 && midturn_compaction_enabled) {
      compact = MaybeCompactDuringTurn(available_schemas, user_input, usage,
                                       turn_start);
    }
    if (compact != MidturnCompact::kNotNeeded) {
      midturn_compaction_enabled = false;
      --step;
      continue;
    }
    if (SteeringState().Requested()) {
      outcome = "steered";
      last_error_ = outcome;
      break;
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
      line_open = false;
      outcome = SteeringState().Requested() ? "steered" : "interrupted";
      last_error_ = outcome;
      printf("\n%s· %s%s\n", YEL(), outcome.c_str(), RST());
      conversation_.Push(
          HarnessMessage("(response interrupted; partial output was "
                         "discarded)"),
          MessageKind::kInternal);
      break;
    }
    if (!r.error.empty()) {
      line_open = false;
      if (DegradeAndRetry(r)) {
        --step;
        continue;
      }
      outcome = "error";
      last_error_ = r.error;
      printf("%s%s%s\n", RED(), TerminalSafe(r.error).c_str(), RST());
      break;
    }

    Usage response_usage;
    response_usage.Add(r.usage);
    if (session_budget > 0 && r.usage.is_object() &&
        !response_usage.cost_reported && !cost_warning_shown_) {
      cost_warning_shown_ = true;
      printf(
          "%s· provider does not report cost; dollar budget is not "
          "enforceable%s\n",
          YEL(), RST());
      DebugLog("cost_unavailable", {{"route", ActiveRoute()}});
    }
    // Providers may buffer a response and deliver it in one SSE burst.
    // Measuring only from the first event to the last therefore reports
    // transport burst speed rather than useful model throughput. Average all
    // output over the successful model-request wall time for this turn instead.
    if (r.duration_ms > 0) {
      model_response_ms += r.duration_ms;
      model_output_tokens += response_usage.output;
    }
    usage.Merge(response_usage);        // this turn's footer
    MergeSessionUsage(response_usage);  // running session totals
    AddRouteUsage(response_usage);
    tool_counts["web_search"] += response_usage.web_searches;
    turn_search_trace_.Add(response_usage.web_searches, r.annotations);
    line_open = !r.suppressed && !r.content.empty() && r.content.back() != '\n';
    if (PrintSearchReceipt(response_usage.web_searches, r.annotations, verbose_,
                           line_open)) {
      line_open = false;
    }
    std::string citations = CitationMarkdown(r.annotations);
    if (!citations.empty() && !r.content.empty()) {
      PrintCitationSources(r.annotations);
      r.content += citations;
      line_open = false;
    }
    if (cost_exceeded()) break;
    // context size = full prompt + what the model just added
    if (r.usage.is_object()) {
      context_policy_.SetReported(
          JsonValue(r.usage, "prompt_tokens", int64_t{0}) +
          JsonValue(r.usage, "completion_tokens", int64_t{0}));
    }
    std::vector<ToolCall> calls = r.tool_calls;
    bool text_mode =
        calls.empty() && !(calls = ParseTextToolCalls(r.content)).empty();

    if (!calls.empty()) {
      if (tool_count + static_cast<int64_t>(calls.size()) > max_tool_calls) {
        last_error_ =
            "tool call limit reached (" + std::to_string(max_tool_calls) + ")";
        outcome = "budget_exceeded";
        printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
        break;
      }
      bool repeated = false;
      for (const ToolCall& call : calls) {
        std::string signature = call.name + "\n" + call.args;
        repeated_calls = signature == last_call ? repeated_calls + 1 : 1;
        last_call = std::move(signature);
        if (repeated_calls > 3) repeated = true;
      }
      if (repeated) {
        last_error_ = "model repeated the same tool call more than 3 times";
        outcome = "budget_exceeded";
        printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
        break;
      }
    }

    if (calls.empty() && r.content.empty()) {
      if (!empty_response_recovered && tool_count > 0) {
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
      outcome = "error";
      last_error_ = "model returned an empty response";
      printf("%s%s%s\n", RED(), last_error_.c_str(), RST());
      break;
    }

    json amsg = {{"role", "assistant"}, {"content", r.content}};
    if (!calls.empty() && !text_mode) {
      json tcs = json::array();
      for (auto& c : calls) {
        tcs.push_back(
            {{"id", c.id},
             {"type", "function"},
             {"function", {{"name", c.name}, {"arguments", c.args}}}});
      }
      amsg["tool_calls"] = tcs;
      api_.PreserveAssistantReasoning(amsg, r);
    }
    conversation_.Push(std::move(amsg), MessageKind::kAssistant);

    if (calls.empty()) {
      ttt_ms = r.first_event_ms;
      // content that looked like a tool call was held back from the
      // stream; if it didn't parse into one, it's prose — show it now
      if (r.suppressed) {
        MdPrint(r.content);
        printf("\n");
      }
      size_t pending = JoinableBackground();
      if (pending) {
        if (line_open) printf("\n");
        line_open = false;
        printf("%s· waiting for %zu background task%s%s\n", DIM(), pending,
               pending == 1 ? "" : "s", RST());
        if (JoinBackgroundOrReport(deadline, usage, max_turn_seconds,
                                   outcome)) {
          continue;
        }
        break;
      }
      complete = true;
      outcome = "complete";
      break;  // plain prose -> turn is done
    }
    if (line_open) printf("\n");
    bool cancelled = RunCalls(calls, text_mode, tool_count, tool_counts, step,
                              deadline, consecutive_failed_tools);
    line_open = false;
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
    if (cancelled || SteeringState().Requested()) BgCancelTasks(processes_);
    if (!cancelled && !SteeringState().Requested() &&
        JoinableBackground() > 0) {
      printf("%s· waiting for delegated work%s\n", DIM(), RST());
      if (!JoinBackgroundOrReport(deadline, usage, max_turn_seconds, outcome)) {
        break;
      }
    }
    if (checkpoint_turn_complete_) {
      complete = true;
      outcome = "checkpoint_prepared";
      break;
    }
    if (SteeringState().Requested() || cancelled) {
      outcome = cancelled ? "interrupted" : "steered";
      if (cancelled) printf("%s· interrupted%s\n", YEL(), RST());
      break;
    }
  }
  if (step >= max_steps) {
    last_error_ = "step limit (" + std::to_string(max_steps) + ") reached";
    std::cout << RED() << "step limit (" << max_steps
              << ") reached — stopping this turn" << RST() << '\n';
  }
  PruneAttachments(turn_start);
  if (complete && !checkpoint_turn_complete_) ArchiveTurnTrace(turn_start);
  if (pending_checkpoint_.is_object() &&
      JsonValue(pending_checkpoint_, "turn", int64_t{-1}) == turn_id_) {
    if (complete && !processes_.PendingCount()) {
      pending_checkpoint_["ready"] = true;
      DebugLog(
          "checkpoint_ready",
          {{"turn", turn_id_},
           {"state_chars",
            JsonValue(pending_checkpoint_, "state", std::string()).size()}});
    } else {
      InvalidatePendingCheckpoint(complete
                                      ? "background work is still active"
                                      : "checkpoint turn did not complete");
    }
  }

  MergeSideUsage(
      usage);  // include side requests that completed in the final step

  double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  double tokens_per_second =
      model_response_ms > 0 ? model_output_tokens * 1000.0 / model_response_ms
                            : 0;
  std::ostringstream stats;
  stats << "· " << FmtCount(usage.input) << " in";
  if (usage.cache_read) {
    stats << " (+" << FmtCount(usage.cache_read) << " cached)";
  }
  if (usage.cache_write) {
    stats << " (+" << FmtCount(usage.cache_write) << " cache write)";
  }
  stats << " · " << FmtCount(usage.output) << " out";
  if (usage.reasoning) {
    stats << " (+" << FmtCount(usage.reasoning) << " reasoning)";
  }
  if (usage.cost > 0) stats << " · " << FmtCost(usage.cost);
  if (usage.web_searches) {
    stats << " · " << usage.web_searches << " search"
          << (usage.web_searches == 1 ? "" : "es");
  }
  if (tool_count) {
    stats << " · " << tool_count << " tool" << (tool_count == 1 ? "" : "s");
  }
  if (tokens_per_second > 0) {
    stats << " · " << std::fixed << std::setprecision(1) << tokens_per_second
          << " tok/s";
  }
  if (ttt_ms >= 0) {
    stats << " · first " << std::fixed << std::setprecision(2)
          << ttt_ms / 1000.0 << 's';
  }
  stats << " · " << std::fixed << std::setprecision(1) << secs << 's';
  std::string stats_line = stats.str();
  std::ostringstream footer;
  footer << (line_open ? "\n" : "") << DIM() << stats_line << RST() << '\n';
  if (api_.render_stream) {
    footer << DIM();
    int64_t separator_width = std::min(
        static_cast<int64_t>(DisplayWidth(stats_line)), TerminalColumns() - 1);
    for (int64_t column = 0; column < separator_width; ++column) {
      footer << "─";
    }
    footer << RST() << '\n';
  }
  std::cout << footer.str();
  DebugLog("turn_end", {{"turn", turn_id_},
                        {"outcome", outcome},
                        {"steps", std::min(step + 1, max_steps)},
                        {"tool_calls", tool_count},
                        {"duration_ms", secs * 1000},
                        {"ttt_ms", ttt_ms},
                        {"tokens_per_second", tokens_per_second},
                        {"usage", UsageJson(usage)},
                        {"session_usage", UsageJson(session_usage_)},
                        {"messages", conversation_.Size()},
                        {"context_tokens", ContextUsed()}});
  checkpoint_hint_active_ = false;
  active_deadline_ = std::chrono::steady_clock::time_point::max();
}

}  // namespace uagent
