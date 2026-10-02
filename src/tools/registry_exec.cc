// Copyright 2026 Timon Gentzsch

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/tools/files.h"
#include "include/tools/shell.h"
#include "src/tools/registry_internal.h"

namespace uagent {

namespace {
// The picture a command rendered arrives with its result, not a read_path
// round later. Only where that read needs no person: anything else stays
// read_path's to ask for.
ToolResult Attached(ToolResult result, const json& a,
                    const ToolContext& context) {
  const std::string attach = JsonValue(a, "attach", "");
  if (!result.Ok() || attach.empty()) return result;
  if (!ApprovalIsYolo() &&
      (PathApprovalRequired(attach, CanonicalCwd()) ||
       PathApprovalClass(attach, PathAccess::kRead) != ApprovalClass::kNone)) {
    result.output +=
        "\n[not attached: " + attach + " needs approval; use read_path]";
    return result;
  }
  result.output += "\n" + ToolReadFile(attach, 1, 0, context.call_id).output;
  return result;
}
}  // namespace

void RegisterExecTools(std::vector<Tool>& tools, ProcessSupervisor& supervisor,
                       const std::filesystem::path& workspace) {
  // The schema below is a raw JSON literal, so its yield bounds cannot be
  // spelled as constants; this assert fails the build if one moves.
  static_assert(kMaxYieldMs == 30000 && kDefaultYieldMs == 10000,
                "update yield_ms in the run schema");
  Tool& run = AddTool(
      tools,
      MakeTool("run",
               "Execute a command in cwd; omit cd. tty=true enables "
               "interactive stdin; detach persists a terminal beyond this "
               "session.",
               json::parse(R"json({"type":"object","properties":{
                    "command":{"type":"string"},
                    "shell":{"type":"string","description":"default bash"},
                    "tty":{"type":"boolean","description":"retain an interactive PTY"},
                    "yield_ms":{"type":"integer","minimum":0,"maximum":30000,
                      "description":"initial wait; 0 blocks to deadline; omitted waits 10000"},
                    "max_output_chars":{"type":"integer","minimum":256,"maximum":65536,
                      "description":"lower per-call returned-output cap"},
                    "detach":{"type":"boolean",
                      "description":"persist terminal and log"}},
                    "required":["command"]})json"),
               [&supervisor](const json& a, const ToolContext& context) {
                 const bool detach = JsonValue(a, "detach", false);
                 return Attached(
                     RunShellCommand(
                         supervisor, context,
                         {.command = JsonValue(a, "command", ""),
                          .shell = JsonValue(a, "shell", "bash"),
                          .background = detach,
                          .detach = detach,
                          .tty = JsonValue(a, "tty", false),
                          .sandbox = JsonValue(a, "sandbox", true),
                          .yield_ms = JsonValue(a, "yield_ms", kDefaultYieldMs),
                          .max_output_chars =
                              JsonValue(a, "max_output_chars", int64_t{0}),
                          .environment_policy =
                              ChildEnvironmentPolicy::kApprovedShell})
                         .result,
                     a, context);
               }));
  // The hatch exists only where there is something to escape. Advertising it
  // unconditionally would spend schema tokens on an argument that does nothing,
  // and invite the model to reach for it on a host that never confined
  // anything. `scratch` and `grep` get none: neither has a use for one.
  if (SandboxEnabled()) {
    run.parameters["properties"]["sandbox"] =
        json{{"type", "boolean"},
             {"description",
              "false runs outside the OS sandbox; always asks a person"}};
    // Mandatory rather than mutating: an approval a person did not give is an
    // approval this must not have. Yolo, remembered grants and headless
    // sessions all fall to a denial, so a delegated child cannot unconfine
    // itself no matter what it was launched with.
    run.approval_class = [](const json& a) {
      return JsonValue(a, "sandbox", true) ? ApprovalClass::kNone
                                           : ApprovalClass::kMandatoryHuman;
    };
    run.mandatory_reason = "runs without the OS sandbox";
  }
  run.clamped_arguments = {"yield_ms", "max_output_chars"};
  run.mutating = true;
  run.capabilities = Capability(ToolCapability::kExecute) |
                     Capability(ToolCapability::kMutate);
  run.validate = [](const json& a) -> std::optional<ToolArgumentIssue> {
    std::string error = RunCommandPolicyError(JsonValue(a, "command", ""));
    if (!error.empty()) {
      return ArgumentIssue("run.command_policy", std::move(error), "command");
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
  run.summary = [](const json& a) { return JsonValue(a, "command", ""); };
  run.header = Verbs("Running", "Ran");
  run.output_view = "tail";
  run.timeout_s = 0;  // bounded by the turn; Escape remains responsive
  // Each call owns its process group and log, so independent commands
  // (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;
  run.command_policy = true;
  run.declared_intent = true;
  const json intent_schema = {
      {"type", "string"},
      {"enum", CommandIntents()},
      {"description", "what it is for; display grouping only"}};
  run.parameters["properties"]["intent"] = intent_schema;
  const json attach_schema = {
      {"type", "string"},
      {"minLength", 1},
      {"description", "image the command writes; shown with the result"}};
  run.parameters["properties"]["attach"] = attach_schema;
  run.present = [](const json& a) {
    json parts = json::array({CommandPart(JsonValue(a, "command", ""))});
    for (json& part : GenericInputParts(a, {"command"})) {
      parts.push_back(std::move(part));
    }
    return parts;
  };

  // ToolRunScratch runs a .py under uv when it is there and falls back to
  // python3 otherwise, so a host with neither can only ever answer this tool
  // with an error. An 800-byte schema that cannot succeed is worse than an
  // absent one, so the tool is omitted on that host. A .sh needs
  // only sh, but gating the whole tool on the interpreter its Python half
  // needs keeps one condition instead of two.
  if (ExecutableOnPath("uv") || ExecutableOnPath("python3")) {
    Tool& python = AddTool(
        tools,
        MakeTool(
            "scratch",
            "Run a one-off script under .uagent/scratch, never requested "
            "project code: a .py with a PEP 723 `# /// script` header runs "
            "under isolated uv, a .sh under sh. Write and fix it with the file "
            "tools, then rerun it with new args instead of resending a long "
            "pipeline through run.",
            json::parse(
                R"json({"type":"object","additionalProperties":false,"properties":{
                    "path":{"type":"string","minLength":1,
                      "description":"the script's path relative to .uagent/scratch"},
                    "args":{"type":"array","items":{"type":"string","maxLength":4096},"maxItems":32,
                      "description":"argv for this run, read from sys.argv or $@"}},
                    "required":["path"]})json"),
            [&supervisor, workspace](const json& a,
                                     const ToolContext& context) {
              return Attached(
                  ToolRunScratch(supervisor, workspace,
                                 JsonValue(a, "path", ""),
                                 JsonValue(a, "args", json(nullptr)), context),
                  a, context);
            }));
    python.declared_intent = true;
    python.parameters["properties"]["intent"] = intent_schema;
    python.parameters["properties"]["attach"] = attach_schema;
    python.mutating = true;
    python.capabilities = Capability(ToolCapability::kExecute) |
                          Capability(ToolCapability::kMutate);
    python.summary = [](const json& a) {
      return JsonValue(a, "path", "") +
             ScratchArgvLabel(JsonValue(a, "args", json(nullptr)));
    };
    python.header = Verbs("Running", "Ran");
    python.output_view = "tail";
    python.present = [](const json& a) {
      return json::array(
          {CommandPart(JsonValue(a, "path", "") +
                       ScratchArgvLabel(JsonValue(a, "args", json(nullptr))))});
    };
    // Running is what a person approves, so they read what will run.
    python.approval_preview = [workspace](const json& a) {
      std::string error;
      const auto script =
          ScratchScriptPath(workspace, JsonValue(a, "path", ""), error);
      if (!script) return error;
      return Utf8Trunc(ReadFile(*script, kPreviewChars + 1).value_or(""),
                       kPreviewChars);
    };
    python.stable_argument = "path";
    python.timeout_s = 0;  // bounded by the turn; no model-driven polling
  }
}

}  // namespace uagent
