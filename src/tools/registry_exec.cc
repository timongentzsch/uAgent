// Copyright 2026 Timon Gentzsch

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
  // The limits in the schemas are filled in from their constants.
  json run_schema = json::parse(R"json({"type":"object","properties":{
                    "command":{"type":"string"},
                    "shell":{"type":"string","description":"default bash"},
                    "tty":{"type":"boolean","description":"retain an interactive PTY"},
                    "yield_ms":{"type":"integer","minimum":0},
                    "max_output_chars":{"type":"integer",
                      "description":"returned-output cap; prefer this over piping through tail/head"},
                    "detach":{"type":"boolean",
                      "description":"persist terminal and log"}},
                    "required":["command"]})json");
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
          "scripts must also return a failing exit status for failed checks.",
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
              if (root_password) spec.secret = root_password(host);
              if (!spec.secret) {
                return ToolFailure(ToolErrorCode::kPermissionDenied,
                                   "the person declined to enter the sudo "
                                   "password");
              }
              spec.nonce = RandomToken(16);
              spec.activity_label = spec.command;
              spec.command =
                  RootCommand(spec.command, host, spec.tty, spec.nonce);
              // sudo cannot gain privileges inside the sandbox; over ssh
              // only the remote end is root.
              if (host.empty()) spec.sandbox = false;
            }
            return RunShellCommand(supervisor, context, std::move(spec)).result;
          }));
  if (root_password) {
    run.parameters["properties"]["root"] = {
        {"type", "boolean"},
        {"description",
         "run as root through sudo; the person types the password, once per "
         "turn. Never call sudo yourself"}};
    run.parameters["properties"]["host"] = {
        {"type", "string"},
        {"description", "with root: user@machine to run it on over ssh"}};
  }
  // The hatch exists only where there is something to escape. Advertising it
  // unconditionally would spend schema tokens on an argument that does nothing,
  // and invite the model to reach for it on a host that never confined
  // anything. `scratch` and `grep` get none: neither has a use for one.
  if (SandboxRuntime().mode != SandboxMode::kOff) {
    run.parameters["properties"]["sandbox"] =
        json{{"type", "boolean"},
             {"description",
              "false runs outside the OS sandbox; always asks a person"}};
  }
  // Mandatory rather than mutating: an approval a person did not give is an
  // approval this must not have. Yolo, remembered grants and headless
  // sessions all fall to a denial, so a delegated child cannot unconfine
  // itself, or become root, no matter what it was launched with.
  run.approval_class = [confined = SandboxRuntime().mode !=
                                   SandboxMode::kOff](const json& a) {
    return JsonValue(a, "root", false) ||
                   (confined && !JsonValue(a, "sandbox", true))
               ? ApprovalClass::kMandatoryHuman
               : ApprovalClass::kNone;
  };
  // What that means, said where it is approved: the sandbox is all that
  // keeps a command from these.
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
    const std::string host = JsonValue(a, "host", "");
    if (a.contains("host") && (!root || host.empty() || host[0] == '-' ||
                               host.find_first_of(" \t\n") != host.npos)) {
      return ArgumentIssue(
          "run.host", "host needs root and names one user@machine", "host");
    }
    if (root && JsonValue(a, "detach", false)) {
      return ArgumentIssue("run.root", "a root command cannot detach",
                           "detach");
    }
    int64_t yield_ms = JsonValue(a, "yield_ms", int64_t{0});
    if (yield_ms > 0 && yield_ms < kMinYieldMs) {
      return ArgumentIssue(
          "run.yield_ms",
          "yield_ms must be 0 or at least " + std::to_string(kMinYieldMs),
          "yield_ms");
    }
    return std::nullopt;
  };
  run.headline = [](const json& a) {
    return Headline{"Running", "Ran", JsonValue(a, "command", "")};
  };
  run.timeout_s = 0;  // bounded by the turn; Escape remains responsive
  // Each call owns its process group and log, so independent commands
  // (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;
  run.declared_intent = true;
  const json intent_schema = {
      {"type", "string"},
      {"enum", CommandIntents()},
      {"description", "what it is for; display grouping only"}};
  run.parameters["properties"]["intent"] = intent_schema;
  run.present = [](const json& a) {
    json parts = json::array({CommandPart(JsonValue(a, "command", ""))});
    for (json& part : GenericInputParts(a, {"command"})) {
      parts.push_back(std::move(part));
    }
    return parts;
  };

  // Offer only installed runtimes; each extension checks its interpreter.
  if (ExecutableOnPath("uv") || ExecutableOnPath("python3") ||
      ExecutableOnPath("node") || ExecutableOnPath("sh")) {
    json scratch_schema = json::parse(
        R"json({"type":"object","additionalProperties":false,"properties":{
                    "path":{"type":"string","minLength":1,
                      "description":"the script's path relative to .uagent/scratch"},
                    "code":{"type":"string","minLength":1,
                      "description":"the script's text: written to path, then run"},
                    "args":{"type":"array","items":{"type":"string"},"maxItems":32,
                      "description":"argv for this run, read from sys.argv or $@"}},
                    "required":["path"]})json");
    json& scratch_properties = scratch_schema["properties"];
    scratch_properties["code"]["maxLength"] = kToolTextArgChars;
    scratch_properties["args"]["items"]["maxLength"] = kToolArgChars;
    Tool& python = AddTool(
        tools,
        MakeTool(
            "scratch",
            "Run a one-off script under .uagent/scratch, never code the "
            "user asked for: a .py with a PEP 723 `# /// script` header runs "
            "under isolated uv, a .sh under sh, .js/.mjs under installed node. "
            "Give code to write the script and run it in this call; without "
            "code the file at path runs "
            "again, with new args, instead of a long pipeline resent through "
            "run.",
            std::move(scratch_schema),
            [&supervisor, workspace](const json& a,
                                     const ToolContext& context) {
              if (a.contains("code")) {
                std::string error;
                const auto script = ScratchScriptPath(
                    workspace, JsonValue(a, "path", ""), error);
                if (!script ||
                    !AtomicWriteFile(script->string(), JsonValue(a, "code", ""),
                                     kSharedFileMode, /*preserve_mode=*/false,
                                     error)) {
                  return ToolFailure(ToolErrorCode::kInvalidArguments, error);
                }
              }
              return ToolRunScratch(
                  supervisor, workspace, JsonValue(a, "path", ""),
                  JsonValue(a, "args", json(nullptr)), context);
            }));
    python.declared_intent = true;
    python.parameters["properties"]["intent"] = intent_schema;
    python.mutating = true;
    python.capabilities = Capability(ToolCapability::kExecute) |
                          Capability(ToolCapability::kMutate);
    python.headline = [](const json& a) {
      return Headline{
          "Running", "Ran",
          JsonValue(a, "path", "") +
              ScratchArgvLabel(JsonValue(a, "args", json(nullptr)))};
    };
    python.present = [](const json& a) {
      return json::array(
          {CommandPart(JsonValue(a, "path", "") +
                       ScratchArgvLabel(JsonValue(a, "args", json(nullptr))))});
    };
    // Running is what a person approves, so they read what will run.
    python.approval_preview = [workspace](const json& a) {
      if (a.contains("code")) {
        return Utf8Trunc(JsonValue(a, "code", ""), kPreviewChars);
      }
      std::string error;
      const auto script =
          ScratchScriptPath(workspace, JsonValue(a, "path", ""), error);
      if (!script) return error;
      return Utf8Trunc(ReadFile(*script, kPreviewChars + 1).value_or(""),
                       kPreviewChars);
    };
    python.timeout_s = 0;  // bounded by the turn; no model-driven polling
  }
}

}  // namespace uagent
