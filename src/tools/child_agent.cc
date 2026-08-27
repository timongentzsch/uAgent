// Copyright 2026 Timon Gentzsch

#include "include/tools/child_agent.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/output_buffer.h"

namespace uagent {
namespace {

constexpr size_t kChildDiagnosticBytes = 2048;

// How far back a child's result record may begin. It is one line at the end of
// the log and mostly trace, so it routinely dwarfs the cap a tool result is
// read through.
constexpr int64_t kEnvelopeRecoveryBytes = int64_t{8} * 1024 * 1024;

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

std::string KnownFailureReason(std::string_view report) {
  const std::string lower = AsciiLower(std::string(report));
  // These are fixed transport/provider categories. Do not fall back to an
  // arbitrary diagnostic line: it can contain a credential, URL query, child
  // command or user data even after terminal escaping.
  if (lower.find("couldn't connect to server") != std::string::npos) {
    return "connection error: Couldn't connect to server";
  }
  if (lower.find("could not resolve host") != std::string::npos) {
    return "connection error: Could not resolve host";
  }
  if (lower.find("ssl connect error") != std::string::npos) {
    return "connection error: SSL connect error";
  }
  if (lower.find("timeout was reached") != std::string::npos ||
      lower.find("request timed out") != std::string::npos) {
    return "request timed out";
  }
  if (lower.find("model_not_found") != std::string::npos ||
      lower.find("model not found") != std::string::npos ||
      lower.find("unsupported-model") != std::string::npos ||
      lower.find("unsupported model") != std::string::npos) {
    return "configured model was rejected";
  }
  if (lower.find("rate limited") != std::string::npos ||
      lower.find("rate_limit") != std::string::npos) {
    return "provider rate limited the request";
  }
  if (lower.find("provider does not report cost") != std::string::npos ||
      lower.find("dollar budget is not enforceable") != std::string::npos) {
    return "provider cost unavailable";
  }
  if (lower.find("session cost limit reached") != std::string::npos ||
      lower.find("session budget") != std::string::npos) {
    return "session cost limit reached";
  }
  if (lower.find("max_tool_calls") != std::string::npos ||
      lower.find("tool call limit") != std::string::npos) {
    return "tool-call limit reached";
  }
  if (lower.find("max_steps") != std::string::npos ||
      lower.find("step limit") != std::string::npos) {
    return "step limit reached";
  }
  if (lower.find("no provider configured") != std::string::npos ||
      lower.find("no usable model") != std::string::npos ||
      lower.find("unknown model route") != std::string::npos) {
    return "route is not usable";
  }
  if (lower.find("cannot spawn shell") != std::string::npos ||
      lower.find("process spawn") != std::string::npos) {
    return "process could not be spawned";
  }
  size_t http = lower.find("http ");
  if (http != std::string::npos && http + 8 <= lower.size() &&
      std::isdigit(Byte(lower[http + 5])) &&
      std::isdigit(Byte(lower[http + 6])) &&
      std::isdigit(Byte(lower[http + 7]))) {
    return "provider HTTP " + lower.substr(http + 5, 3);
  }
  return {};
}

std::string FailureSummary(ChildAgentFailureStage stage,
                           std::string_view diagnostics) {
  std::string reason = KnownFailureReason(diagnostics);
  if (reason.empty()) reason = "failed; see retained diagnostics";
  return Utf8Trunc(std::string(FailureStageName(stage)) + ": " + reason,
                   size_t{180});
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
    if (const json* error =
            envelope->contains("error") ? &(*envelope)["error"] : nullptr;
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
  std::string summary_source = reported;
  if (!summary_source.empty()) summary_source += '\n';
  summary_source.append(diagnostics);
  std::string report =
      "error: " + FailureSummary(stage, summary_source) +
      "\ndelegated child failed\nconfigured route: " + configured +
      "\nfailure stage: " + FailureStageName(stage);
  if (!reported.empty()) {
    report += "\nchild reported: " + TerminalSafe(reported);
  }
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
                       start == std::string::npos ? end : end - start - 1);
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

std::string ChildAgentRecoverEnvelope(std::string output,
                                     const std::string& log_path) {
  if (log_path.empty() || ChildAgentEnvelope(output)) return output;
  std::ifstream file(log_path, std::ios::binary | std::ios::ate);
  if (!file) return output;
  auto size = static_cast<int64_t>(file.tellg());
  file.seekg(std::max(int64_t{0}, size - kEnvelopeRecoveryBytes));
  std::string whole((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
  if (std::optional<json> envelope = ChildAgentEnvelope(whole)) {
    // Recovered, not re-read: the diagnostics keep the cap they were given,
    // and only the record the caller is owed is added back.
    output += "\n" + JsonDump(*envelope);
  }
  return output;
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

std::string ChildAgentConstraintNotes(const std::vector<std::string>& clamped) {
  std::string notes;
  for (const std::string& one : clamped) notes += "\n[clamped " + one + "]";
  return notes;
}

std::string ChildAgentAnswer(std::string output,
                             const std::vector<std::string>& clamped) {
  std::string note = ChildAgentConstraintNotes(clamped);
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
