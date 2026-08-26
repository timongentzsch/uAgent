// Copyright 2026 Timon Gentzsch

#include "include/tools/child_agent.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/output_buffer.h"

namespace uagent {
namespace {

constexpr size_t kChildDiagnosticBytes = 2048;

const char* FailureStageName(ChildAgentFailureStage stage) {
  switch (stage) {
    case ChildAgentFailureStage::kRouteResolution:
      return "route resolution";
    case ChildAgentFailureStage::kSpawn:
      return "process spawn";
    case ChildAgentFailureStage::kExecution:
      return "child execution";
  }
  return "child execution";
}

const char* FailureRemedy(ChildAgentFailureStage stage) {
  switch (stage) {
    case ChildAgentFailureStage::kRouteResolution:
      return "choose a configured model route or correct UAGENT_PROVIDERS";
    case ChildAgentFailureStage::kSpawn:
      return "verify the uagent executable and local process limits, then "
             "retry this route";
    case ChildAgentFailureStage::kExecution:
      return "inspect the partial diagnostics and verify this endpoint and "
             "model before retrying";
  }
  return "inspect the partial diagnostics before retrying";
}

}  // namespace

std::string ChildAgentFailureReport(std::string_view route,
                                    ChildAgentFailureStage stage,
                                    std::string_view diagnostics) {
  // A child that ran and stopped on a limit reports which one, before the
  // diagnostics are squeezed: that line is what tells the caller whether
  // raising a ceiling would change anything.
  std::string stopped;
  std::string answer;
  std::string reported;
  if (std::optional<json> envelope =
          ChildAgentEnvelope(std::string(diagnostics))) {
    stopped = ChildAgentStopNote(JsonValue(*envelope, "stop", json()));
    answer = JsonValue(*envelope, "answer", std::string());
    if (const json* error = envelope->contains("error") ? &(*envelope)["error"]
                                                        : nullptr;
        error && error->is_string()) {
      reported = error->get<std::string>();
    }
  }
  // Whatever the child printed besides its envelope. The envelope's own
  // content is rendered above as the reported error, the stop and the partial
  // answer, so repeating it here would spend the diagnostics budget on bytes
  // the reader has already been given.
  std::string rest;
  for (std::string_view remaining = diagnostics; !remaining.empty();) {
    size_t brk = remaining.find('\n');
    std::string_view line = remaining.substr(0, brk);
    remaining = brk == std::string_view::npos ? std::string_view()
                                              : remaining.substr(brk + 1);
    if (line.starts_with('{') &&
        line.find("uagent.headless.v1") != std::string_view::npos) {
      continue;
    }
    if (!rest.empty()) rest += "\n";
    rest.append(line);
  }
  HeadTailBuffer bounded(kChildDiagnosticBytes);
  bounded.Push(TerminalSafe(rest));
  std::string partial = bounded.Snapshot();
  if (Trim(partial).empty()) partial = "(none captured)";
  std::string configured = route.empty() ? "(unresolved)" : TerminalSafe(route);
  std::string report = "error: delegated child failed\nconfigured route: " +
                       configured + "\nfailure stage: " +
                       FailureStageName(stage);
  if (!reported.empty()) report += "\nchild reported: " + TerminalSafe(reported);
  if (!stopped.empty()) report += "\n" + stopped;
  if (!answer.empty()) {
    report += "\npartial answer:\n" + TerminalSafe(answer);
  }
  report += "\nremedy: " + std::string(FailureRemedy(stage)) +
            "\nfallback: none; provider, model, pricing, and privacy policy "
            "were not changed\npartial diagnostics:\n" +
            partial;
  return report;
}

EnvironmentOverrides ChildAgentEnvironment(SideRoute route) {
  return {
      {"UAGENT_DEPTH", std::to_string(AgentDepth() + 1)},
      {"UAGENT_BASE_URL", std::move(route.base_url)},
      {"UAGENT_API_KEY", std::move(route.api_key)},
      {"UAGENT_MODEL", std::move(route.model)},
      {"UAGENT_CONTEXT", std::to_string(route.context)},
      {"UAGENT_REASONING_EFFORT", std::move(route.effort)},
      {"UAGENT_OPENROUTER_COMPATIBLE",
       route.protocol == ProviderProtocol::kOpenRouter ? "1" : "0"},
      {"UAGENT_PROVIDER_PROTOCOL", ProviderProtocolName(route.protocol)},
      {"UAGENT_WIRE_API", WireApiName(route.wire_api)},
      {"UAGENT_HOSTED_TOOLS", route.hosted_web_search ? "web_search" : ""},
      {"UAGENT_OPENROUTER_VARIANT", std::move(route.variant)},
      {"UAGENT_USAGE_FILE", UsageLedger()},
  };
}

std::string ChildAgentCommand(bool debug, const std::string& prompt) {
  return ShellQuote(ExecutablePath()) + " --yolo --json" +
         (debug ? " --debug" : "") + " -p " + ShellQuote(prompt);
}

// The child prints its envelope as one line. Progress lines precede it, the
// exit-code note and any captured-log hint follow it, so the search runs
// backwards over whole lines rather than to the end of the text: parsing from
// the last `{` to the end fails on whatever the harness appended after it.
std::optional<json> ChildAgentEnvelope(const std::string& output) {
  size_t end = output.size();
  while (end > 0) {
    size_t start = output.rfind('\n', end - 1);
    std::string_view line(output);
    line = line.substr(start == std::string::npos ? 0 : start + 1,
                       start == std::string::npos
                           ? end
                           : end - start - 1);
    if (line.starts_with('{')) {
      json envelope = json::parse(line, nullptr, false);
      if (!envelope.is_discarded() && envelope.is_object() &&
          JsonValue(envelope, "schema", std::string()) ==
              "uagent.headless.v1") {
        return envelope;
      }
    }
    if (start == std::string::npos) break;
    end = start;
  }
  return std::nullopt;
}

std::string ChildAgentStopNote(const json& stop) {
  std::string reason = JsonValue(stop, "reason", std::string());
  if (reason.empty() || reason == "completed") return {};
  std::string note = "[child stopped: " + reason;
  int64_t steps = JsonValue(stop, "steps", int64_t{0});
  int64_t calls = JsonValue(stop, "tool_calls", int64_t{0});
  note += " after " + std::to_string(steps) + " step(s), " +
          std::to_string(calls) + " tool call(s), " +
          FmtCost(JsonValue(stop, "cost", 0.0));
  std::string detail = JsonValue(stop, "detail", std::string());
  if (!detail.empty()) note += "; " + detail;
  // The point of naming the limit is that the caller can decide between
  // raising it and living with what came back.
  note +=
      "; rerun with that ceiling raised, or use this partial result as it is]";
  return note;
}

std::string ChildAgentAnswer(std::string output,
                             const std::vector<std::string>& clamped) {
  std::string note;
  for (const std::string& one : clamped) {
    note += "\n[clamped " + one + "]";
  }
  std::optional<json> envelope = ChildAgentEnvelope(output);
  if (!envelope) {
    // Saying so beats presenting the raw stream as though it were the answer.
    return std::move(output) + note +
           "\n[child produced no result envelope; the text above is its raw "
           "output]";
  }
  std::string answer = JsonValue(*envelope, "answer", std::string());
  std::string stop = ChildAgentStopNote(JsonValue(*envelope, "stop", json()));
  if (!stop.empty()) note += "\n" + stop;
  return answer + note;
}

std::optional<ToolResult> ChildAgentBudgetBlock(
    const Api& api, const ProcessSupervisor& processes, double& remaining) {
  remaining = api.config.session_budget - api.session_cost;
  if (api.config.session_budget <= 0) return std::nullopt;
  if (remaining <= 0) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: session cost limit reached");
  }
  if (processes.JoinableCount() > 0) {
    return ToolFailure(
        ToolErrorCode::kLimitExceeded,
        "error: budgeted child already running; wait for its result");
  }
  return std::nullopt;
}

}  // namespace uagent
