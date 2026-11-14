// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/sandbox.h"
#include "include/core/strings.h"
#include "include/tools/shell.h"
#include "src/tools/registry_internal.h"

namespace uagent {

void RegisterExecTools(std::vector<Tool>& tools, ActivitySupervisor& supervisor,
                       const std::filesystem::path& workspace,
                       const RootPassword& root_password) {
  json run_schema = json::parse(R"json({"type":"object","properties":{
                    "command":{"type":"string"},
                    "description":{"type":"string",
                      "description":"what it does in a few words, imperative: Run the unit tests"},
                    "shell":{"type":"string","description":"default bash"},
                    "tty":{"type":"boolean","description":"retain an interactive PTY"},
                    "yield_ms":{"type":"integer","minimum":0},
                    "max_output_chars":{"type":"integer",
                      "description":"returned-output cap; prefer this over piping through tail/head"},
                    "detach":{"type":"boolean",
                      "description":"persist terminal and log"}},
                    "required":["command","description"]})json");
  json& run_properties = run_schema["properties"];
  run_properties["yield_ms"]["maximum"] = kMaxYieldMs;
  run_properties["yield_ms"]["description"] =
      "initial wait; 0 blocks to deadline; omitted waits " +
      std::to_string(kDefaultYieldMs);
  run_properties["max_output_chars"]["minimum"] = kToolMinOutputChars;
  run_properties["max_output_chars"]["maximum"] = kToolTextArgChars;
  Tool& run = AddTool(
      tools,
      MakeTool(
          "run",
          "Execute a command in cwd; omit cd. tty=true enables "
          "interactive stdin; detach persists a terminal beyond this "
          "session. Bash/zsh pipelines propagate failures (pipefail); nested "
          "scripts must also return a failing exit status for failed checks. "
          "A command still running when this returns becomes an activity "
          "whose result starts your next turn when it ends: end the turn "
          "instead of waiting unless your next step needs it; to check back "
          "later, start sleep N this way.",
          std::move(run_schema),
          [&supervisor, root_password](const json& a,
                                       const ToolContext& context) {
            const bool detach = JsonValue(a, "detach", false);
            ShellCommand spec{
                .command = JsonValue(a, "command", ""),
                .shell = JsonValue(a, "shell", "bash"),
                .background = detach,
                .detach = detach,
                .tty = JsonValue(a, "tty", false),
                .sandbox = JsonValue(a, "sandbox", true),
                .yield_ms = JsonValue(a, "yield_ms", kDefaultYieldMs),
                .max_output_chars =
                    JsonValue(a, "max_output_chars", int64_t{0}),
                .environment_policy = ChildEnvironmentPolicy::kApprovedShell};
            if (JsonValue(a, "root", false)) {
              const std::string host = JsonValue(a, "host", "");
              // A password only when sudo needs one: already root here
              // needs no sudo, and a user sudo lets in (NOPASSWD) none.
              if (!host.empty() || geteuid() != 0) {
                if (!PasswordlessSudo(host)) {
                  if (root_password) spec.secret = root_password(host);
                  if (!spec.secret) {
                    return ToolFailure(ToolErrorCode::kPermissionDenied,
                                       "the person declined to enter the "
                                       "sudo password");
                  }
                  spec.nonce = RandomToken(16);
                }
                spec.activity_label = spec.command;
                spec.command =
                    RootCommand(spec.command, host, spec.tty, spec.nonce);
              }
              // sudo cannot gain privileges inside the sandbox; over ssh
              // only the remote end is root.
              if (host.empty()) spec.sandbox = false;
            } else if (root_password) {
              // A bash function named sudo says what to use, wherever bash
              // meets it, instead of waiting on a prompt nobody answers.
              spec.environment.emplace_back(
                  "BASH_FUNC_sudo%%",
                  "() { echo 'use run root=true (host=user@machine for "
                  "another machine) instead of sudo' >&2; return 1; }");
            }
            return RunShellCommand(supervisor, context, std::move(spec)).result;
          }));
  if (root_password) {
    run.parameters["properties"]["root"] = {
        {"type", "boolean"},
        {"description",
         "run as root through sudo; when sudo needs a password the person "
         "types it, once per turn. Never call sudo yourself"}};
    run.parameters["properties"]["host"] = {
        {"type", "string"},
        {"description", "with root: user@machine to run it on over ssh"}};
  }
  // Offered only where there is a sandbox to leave: elsewhere the argument
  // would do nothing.
  if (SandboxRuntime().mode != SandboxMode::kOff) {
    run.parameters["properties"]["sandbox"] =
        json{{"type", "boolean"},
             {"description",
              "false runs outside the OS sandbox; always asks a person"}};
  }
  // Mandatory, not mutating: yolo, remembered grants and headless sessions
  // all end in a denial, so a delegated child cannot unconfine itself or
  // become root.
  run.approval_class = [confined = SandboxRuntime().mode !=
                                   SandboxMode::kOff](const json& a) {
    return JsonValue(a, "root", false) ||
                   (confined && !JsonValue(a, "sandbox", true))
               ? ApprovalClass::kMandatoryHuman
               : ApprovalClass::kNone;
  };
  run.mandatory_reason = [](const json& a) -> std::string {
    if (!JsonValue(a, "root", false)) {
      return "runs without the OS sandbox: it can reach saved logins, "
             "browser sessions and settings";
    }
    const std::string host = JsonValue(a, "host", "");
    return host.empty() ? "runs as root on this machine"
                        : "runs as root on " + host;
  };
  run.clamped_arguments = {"yield_ms", "max_output_chars"};
  run.mutating = true;
  run.capabilities = Capability(ToolCapability::kExecute) |
                     Capability(ToolCapability::kMutate);
  run.validate = [](const json& a) -> std::optional<ToolArgumentIssue> {
    const bool root = JsonValue(a, "root", false);
    // An empty host is one not given: models that fill every optional
    // argument send "".
    const std::string host = JsonValue(a, "host", "");
    if (!host.empty() &&
        (!root || host[0] == '-' || host.find_first_of(" \t\n") != host.npos)) {
      return ArgumentIssue(
          "run.host", "host needs root and names one user@machine", "host");
    }
    if (root && JsonValue(a, "detach", false)) {
      return ArgumentIssue("run.root", "a root command cannot detach",
                           "detach");
    }
    return std::nullopt;
  };
  // The command stays the target: approvals and summaries read that one.
  run.headline = [](const json& a) {
    constexpr size_t kDescriptionChars = 120;
    return Headline{"Running", "Ran", JsonValue(a, "command", ""),
                    Utf8Prefix(FirstLine(Trim(JsonValue(a, "description", ""))),
                               kDescriptionChars)};
  };
  run.timeout_s = 0;  // bounded by the turn; Escape remains responsive
  // Each call owns its process group and log, so independent commands
  // (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;
  run.present = [](const json& a) {
    json parts = json::array({CommandPart(JsonValue(a, "command", ""))});
    for (json& part : GenericInputParts(a, {"command", "description"})) {
      parts.push_back(std::move(part));
    }
    return parts;
  };

  // Running is what a person approves, so a script the agent wrote for it
  // is shown under the command that starts it.
  run.approval_preview = [workspace](const json& a) {
    std::string preview = JsonValue(a, "command", "");
    const std::filesystem::path scratch =
        CanonicalAccessPath(ScratchDir(workspace).string());
    std::filesystem::path script;
    size_t named = 0;
    for (size_t at = 0; at < preview.size();) {
      const size_t end =
          std::min(preview.find_first_of(" \t\n", at), preview.size());
      std::string word = preview.substr(at, end - at);
      std::erase_if(word, [](char c) { return c == '\'' || c == '"'; });
      at = end + 1;
      if (word.find(".uagent/scratch/") == std::string::npos) continue;
      script = CanonicalAccessPath((workspace / word).string());
      ++named;
    }
    std::error_code ec;
    if (named != 1 || !PathWithin(script, scratch) ||
        !std::filesystem::is_regular_file(script, ec)) {
      return preview;
    }
    return preview + "\n\n" + script.filename().string() + ":\n" +
           Utf8Trunc(ReadFile(script, kPreviewChars + 1).value_or(""),
                     kPreviewChars);
  };
}

}  // namespace uagent
