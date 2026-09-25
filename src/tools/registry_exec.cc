// Copyright 2026 Timon Gentzsch

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/tools/shell.h"
#include "src/tools/registry_internal.h"

namespace uagent {

void RegisterExecTools(std::vector<Tool>& tools, ProcessSupervisor& supervisor,
                       const std::filesystem::path& workspace) {
  auto schema = [](const char* s) { return json::parse(s); };
  // The schema below is a raw JSON literal, so its "maximum" cannot be spelled
  // as kMaxYieldMs directly; this assert fails the build if the constant moves.
  static_assert(kMaxYieldMs == 30000, "update \"maximum\" in the run schema");
  Tool& run = AddTool(
      tools,
      MakeTool("run",
               "Execute a command in cwd; omit cd. Use the project's Python "
               "runner (uv run/pytest). tty=true enables interactive stdin; "
               "detach persists a terminal beyond this session.",
               schema(R"json({"type":"object","properties":{
                    "command":{"type":"string"},
                    "shell":{"type":"string","description":"default bash"},
                    "tty":{"type":"boolean","description":"retain an interactive PTY"},
                    "yield_ms":{"type":"integer","minimum":0,"maximum":30000,
                      "description":"initial wait; 0 blocks to deadline; omitted uses UAGENT_RUN_YIELD_MS"},
                    "max_output_chars":{"type":"integer","minimum":256,"maximum":65536,
                      "description":"lower per-call returned-output cap"},
                    "detach":{"type":"boolean",
                      "description":"persist terminal and log"}},
                    "required":["command"]})json"),
               [&supervisor](const json& a, const ToolContext& context) {
                 return ToolRunApprovedShell(
                     supervisor, JsonValue(a, "command", ""), context,
                     JsonValue(a, "detach", false),
                     JsonValue(a, "shell", "bash"), JsonValue(a, "tty", false),
                     JsonValue(a, "yield_ms", RunDefaultYieldMs()),
                     JsonValue(a, "max_output_chars", int64_t{0}),
                     JsonValue(a, "sandbox", true));
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
    // sessions all fall to a denial, so a collaborator child cannot unconfine
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
      {"enum", json::array({"explore", "change", "execute"})},
      {"description",
       "Activity intent only; does not change permissions. Default execute."}};
  run.parameters["properties"]["intent"] = intent_schema;
  const json description_schema = {
      {"type", json::array({"string", "null"})},
      {"description",
       "Optional short action label, e.g. Running tests. Display only."}};
  run.present = [](const json& a) {
    json parts = json::array({CommandPart(JsonValue(a, "command", ""))});
    for (json& part : GenericInputParts(a, {"command"})) {
      parts.push_back(std::move(part));
    }
    return parts;
  };
  run.parameters["properties"]["description"] = description_schema;

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
            "Run a one-off script, never requested project code. Write it "
            "under .uagent/scratch with write_file (a .py declares its "
            "dependencies in a PEP 723 `# /// script` header and runs under "
            "isolated uv; a .sh runs under sh), fix it with edit_file, and "
            "rerun it here with different `args` instead of rewriting it. "
            "Prefer this over resending a long pipeline or heredoc through "
            "run.",
            schema(
                R"json({"type":"object","additionalProperties":false,"properties":{
                    "path":{"type":"string","minLength":1,
                      "description":"the script's path relative to .uagent/scratch"},
                    "args":{"type":"array","items":{"type":"string","maxLength":4096},"maxItems":32,
                      "description":"argv for this run, read from sys.argv or $@"}},
                    "required":["path"]})json"),
            [&supervisor, workspace](const json& a,
                                     const ToolContext& context) {
              return ToolRunScratch(
                  supervisor, workspace, JsonValue(a, "path", ""),
                  JsonValue(a, "args", json(nullptr)), context);
            }));
    python.declared_intent = true;
    python.parameters["properties"]["intent"] = intent_schema;
    python.parameters["properties"]["description"] = description_schema;
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
      std::ifstream input(*script);
      std::string source((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
      return Utf8Trunc(source, kPreviewChars);
    };
    python.stable_argument = "path";
    python.timeout_s = 0;  // bounded by the turn; no model-driven polling
  }
}

}  // namespace uagent
