// Copyright 2026 Timon Gentzsch

#include "include/tools/child_agent.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/tools/files.h"
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

const std::string& CollaboratorSessionFile() {
  static const std::string kSessionFile = [] {
    std::string requested = EnvStr("UAGENT_INTERNAL_SESSION_FILE");
    if (requested.empty()) return std::string();
    std::filesystem::path root =
        CanonicalAccessPath(UagentDir("collaborators"));
    std::filesystem::path file = CanonicalAccessPath(requested);
    if (!PathWithin(file, root)) return std::string();
    return file.string();
  }();
  return kSessionFile;
}

namespace {

// `.mail-` cannot collide with another collaborator's record: an id passes
// through SafeFileComponent, whose alphabet has no dot. Flat siblings rather
// than a subdirectory because the pruner removes files, not directories, and
// grouping mail with its record then costs one substring search.
constexpr std::string_view kMailInfix = ".mail-";

std::string CollaboratorDir() { return UagentDir("collaborators"); }

// Sorted oldest first. The name carries a UTC second, the writing pid and a
// per-process counter, so messages from one parent stay ordered and two
// parents in the same second interleave by pid rather than by nothing.
std::vector<std::filesystem::path> CollaboratorMailFiles(
    const std::string& id) {
  std::vector<std::filesystem::path> files;
  if (id.empty() || SafeFileComponent(id) != id) return files;
  const std::string prefix = id + std::string(kMailInfix);
  std::error_code error;
  for (std::filesystem::directory_iterator it(CollaboratorDir(), error), end;
       !error && it != end; it.increment(error)) {
    std::string name = it->path().filename().string();
    if (name.starts_with(prefix) && name.ends_with(".json")) {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// The prompt, or nothing when the file is unreadable or malformed. Either way
// the caller unlinks: mail that cannot be delivered would otherwise be retried
// on every step for as long as the record survives.
std::optional<std::string> ReadCollaboratorMail(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  json mail = json::parse(input, nullptr, false);
  if (mail.is_discarded() || !mail.is_object()) return std::nullopt;
  std::string prompt = JsonValue(mail, "prompt", std::string());
  if (prompt.empty()) return std::nullopt;
  return prompt;
}

// The id this process is running as, from the session file the parent handed
// down. Empty in a process that is not a collaborator.
std::string OwnCollaboratorId() {
  const std::string& session = CollaboratorSessionFile();
  if (session.empty()) return {};
  std::string name = std::filesystem::path(session).filename().string();
  constexpr std::string_view kSuffix = ".session.json";
  if (!name.ends_with(kSuffix)) return {};
  name.resize(name.size() - kSuffix.size());
  return name;
}

}  // namespace

ToolResult WriteCollaboratorMail(const std::string& id,
                                 const std::string& prompt) {
  static std::atomic<uint64_t> sequence{0};
  // Zero-padded so a plain filename sort is a chronological one within the
  // second the stamp resolves to.
  std::string seq =
      std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) % 10000);
  seq.insert(0, 4 - std::min<size_t>(4, seq.size()), '0');
  std::string path = CollaboratorDir() + "/" + id + std::string(kMailInfix) +
                     UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
                     std::to_string(getpid()) + "-" + seq + ".json";
  json mail = {{"format", 1}, {"prompt", prompt}};
  return ToolAtomicWrite(path, JsonDump(mail, 2) + "\n", kPrivateFileMode,
                         /*preserve_mode=*/true);
}

std::vector<std::string> TakeCollaboratorMail(const std::string& id) {
  std::vector<std::string> prompts;
  for (const std::filesystem::path& path : CollaboratorMailFiles(id)) {
    std::optional<std::string> prompt = ReadCollaboratorMail(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    if (prompt) prompts.push_back(std::move(*prompt));
  }
  return prompts;
}

void DrainCollaboratorMailIntoSteering() {
  const std::string id = OwnCollaboratorId();
  if (id.empty()) return;
  for (const std::filesystem::path& path : CollaboratorMailFiles(id)) {
    std::optional<std::string> prompt = ReadCollaboratorMail(path);
    // Queued before the unlink, so a crash in between costs a repeat rather
    // than the message. The reverse order would lose it outright.
    if (prompt) SteeringState().Queue("[parent guidance]\n" + *prompt);
    std::error_code error;
    std::filesystem::remove(path, error);
  }
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
                       "error: session cost limit reached");
  }
  if (api.config.session_token_budget > 0 && remaining_tokens <= 0) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: session generated-token limit reached");
  }
  if ((api.config.session_budget > 0 || api.config.session_token_budget > 0) &&
      processes.JoinableCount() > 0) {
    return ToolFailure(
        ToolErrorCode::kLimitExceeded,
        "error: budgeted child already running; wait for its result");
  }
  return std::nullopt;
}

}  // namespace uagent
