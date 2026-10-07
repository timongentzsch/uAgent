// Copyright 2026 Timon Gentzsch

#include "include/agent/child_agent.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/file_services.h"
#include "include/agent/session_links.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/output_buffer.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"

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

// Fixed transport/provider categories, each with the report fragments that
// identify it. Never an arbitrary diagnostic line: it can contain a
// credential, URL query, child command or user data even after terminal
// escaping.
struct FailureCategory {
  const char* reason;
  std::string_view fragments[4];
};
constexpr FailureCategory kFailureCategories[] = {
    {"connection error: Couldn't connect to server",
     {"couldn't connect to server"}},
    {"connection error: Could not resolve host", {"could not resolve host"}},
    {"connection error: SSL connect error", {"ssl connect error"}},
    {"request timed out", {"timeout was reached", "request timed out"}},
    {"configured model was rejected",
     {"model_not_found", "model not found", "unsupported-model",
      "unsupported model"}},
    {"provider rate limited the request", {"rate limited", "rate_limit"}},
    {"provider cost unavailable",
     {"provider does not report cost", "dollar budget is not enforceable"}},
    {"session cost limit reached",
     {"session cost limit reached", "session budget"}},
    {"tool-call limit reached", {"max_tool_calls", "tool call limit"}},
    {"step limit reached", {"max_steps", "step limit"}},
    {"route is not usable",
     {"no provider configured", "no usable model", "unknown model route"}},
    {"process could not be spawned", {"cannot spawn shell", "process spawn"}},
};

std::string KnownFailureReason(std::string_view report) {
  const std::string lower = AsciiLower(std::string(report));
  for (const FailureCategory& category : kFailureCategories) {
    for (std::string_view fragment : category.fragments) {
      if (!fragment.empty() && lower.find(fragment) != std::string::npos) {
        return category.reason;
      }
    }
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
  // Diagnostics often arrive as a failed result's output; its marker is the
  // carrier's, not part of what the child printed.
  if (diagnostics.starts_with(kToolErrorPrefix)) {
    diagnostics.remove_prefix(kToolErrorPrefix.size());
  }
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
    reported = JsonValue(*envelope, "error", "");
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
  EnvironmentOverrides environment = RouteEnvironment(route);
  environment.emplace_back("UAGENT_INTERNAL_DEPTH",
                           std::to_string(AgentDepth() + 1));
  environment.emplace_back("UAGENT_API_KEY", std::move(route.api_key));
  environment.emplace_back("UAGENT_INTERNAL_USAGE_FILE", UsageLedger());
  // A child is never less confined than the session that delegates to it:
  // it runs under the sandbox this process actually has (fixed at start), not
  // one it composes from a configuration that may have been loosened since.
  environment.emplace_back("UAGENT_INTERNAL_SANDBOX", SandboxInheritance());
  return environment;
}

std::vector<std::string> ChildAgentCommand(bool debug,
                                           const std::string& prompt,
                                           const std::string& model) {
  std::vector<std::string> argv = {ExecutablePath(), "--yolo", "--json"};
  if (debug) argv.emplace_back("--debug");
  argv.insert(argv.end(), {"--model", model, "-p", prompt});
  return argv;
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
  const std::string tail = ReadFileTail(log_path, kEnvelopeRecoveryBytes);
  if (std::optional<json> envelope = ChildAgentEnvelope(tail)) {
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

const std::string& DelegatedSessionFile() {
  static const std::string kSessionFile = [] {
    std::string requested = EnvStr("UAGENT_INTERNAL_SESSION_FILE");
    if (requested.empty()) return std::string();
    std::filesystem::path root = CanonicalAccessPath(UagentDir(kHistoryDir));
    std::filesystem::path file = CanonicalAccessPath(requested);
    if (!PathWithin(file, root)) return std::string();
    return file.string();
  }();
  return kSessionFile;
}

const json& OwnDelegation() {
  static const json kDelegation = [] {
    if (DelegatedSessionFile().empty()) return json::object();
    json parsed =
        json::parse(EnvStr("UAGENT_INTERNAL_DELEGATION"), nullptr, false);
    return parsed.is_object() ? parsed : json::object();
  }();
  return kDelegation;
}

std::string OwnSessionFile() {
  const std::string& delegated = DelegatedSessionFile();
  if (!delegated.empty()) return delegated;
  return EnvStr("UAGENT_INTERNAL_SESSION_PATH");
}

std::string OwnSessionId() {
  const std::string file = OwnSessionFile();
  if (file.empty()) return {};
  std::filesystem::path path(file);
  return path.extension() == ".json" ? path.stem().string() : std::string();
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
    const Api& api, const ProcessSupervisor& processes, double& remaining_cost,
    int64_t& remaining_tokens) {
  remaining_cost = api.config.session_budget - api.session_cost;
  remaining_tokens =
      api.config.session_token_budget > api.session_generated_tokens
          ? api.config.session_token_budget - api.session_generated_tokens
          : 0;
  if (api.config.session_budget > 0 && remaining_cost <= 0) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "session cost limit reached");
  }
  if (api.config.session_token_budget > 0 && remaining_tokens <= 0) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "session generated-token limit reached");
  }
  if ((api.config.session_budget > 0 || api.config.session_token_budget > 0) &&
      processes.JoinableCount() > 0) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "budgeted child already running; wait for its result");
  }
  return std::nullopt;
}

std::string DelegationRuntimeContext(const Api& api) {
  // No provider list reaches here; the built-in templates still scope the
  // common routes, and a custom endpoint degrades to a bare model id.
  return "[delegation: parent=" + TerminalSafe(RouteSelection(api, {})) +
         "; default=parent]";
}

}  // namespace uagent
