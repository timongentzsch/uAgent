// Copyright 2026 Timon Gentzsch

#include "include/tools/registry.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/media.h"
#include "include/tools/adapt_system.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/tools/path_policy.h"
#include "include/tools/shell.h"

namespace uagent {

std::vector<Tool> BuiltinTools(ProcessSupervisor& supervisor,
                               const std::filesystem::path& workspace,
                               bool inline_images,
                               AdaptiveSystemState* adaptive_system) {
  auto schema = [](const char* s) { return json::parse(s); };
  std::vector<Tool> tools;
  if (adaptive_system) tools.push_back(AdaptSystemTool(*adaptive_system));
  auto path_tool = [&](Tool tool) -> Tool& {
    tool.needs_approval = [workspace](const json& args) {
      return PathApprovalRequired(JsonValue(args, "path", ""), workspace);
    };
    return AddTool(tools, std::move(tool));
  };

  // Reading a file and listing a directory are the same act — show me what is
  // at this path — and took the same three arguments as separate tools.
  Tool& read = path_tool(MakeTool(
      "read_path",
      "Read what is at a path: a text file's contents or line range, or a "
      "directory's entries. Results remain in context; do not reread an "
      "unchanged range. Reread after edits or external changes when exact "
      "current text matters. Use grep when the file or symbol is unknown; "
      "batch independent paths.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "offset":{"type":"integer","description":"first line or entry (default 1)"},
                    "limit":{"type":"integer","description":"lines or entries (default 1000)"}},
                    "required":["path"]})json"),
      [workspace](const json& a, const ToolContext&) {
        std::string path = JsonValue(a, "path", ".");
        int64_t offset = JsonValue(a, "offset", int64_t{0});
        int64_t limit = JsonValue(a, "limit", int64_t{0});
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
          return ToolListDir(path, offset, limit,
                             !PathApprovalRequired(path, workspace));
        }
        return ToolReadFile(path, offset > 0 ? offset : 1, limit);
      }));
  read.parallel_safe = true;
  read.capabilities = Capability(ToolCapability::kInspect);
  read.dedupe_output = true;
  // Reads get a larger, contiguous window than logs and remote output. This
  // avoids paying another model round merely to continue an ordinary source
  // file while keeping every other tool on the global result cap.
  read.result_chars = ReadFileResultChars();

  // Creating and editing are the same act at different granularity, so one
  // tool covers both: content replaces the file, edits change part of it.
  Tool& edit = path_tool(MakeTool(
      "edit_file",
      "Change a file: edits applies exact search/replace, batched and atomic "
      "in order; content creates the file or replaces it whole. Prefer edits "
      "on an existing file.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "content":{"type":"string",
                      "description":"whole new contents; omit when using edits"},
                    "edits":{"type":"array","minItems":1,"maxItems":64,
                      "description":"one or more exact replacements in order",
                      "items":{"type":"object","properties":{
                        "old":{"type":"string"},"new":{"type":"string"},
                        "replace_all":{"type":"boolean"}},
                        "required":["old","new"],"additionalProperties":false}}},
                    "required":["path"]})json"),
      [](const json& a, const ToolContext&) {
        std::string path = JsonValue(a, "path", "");
        if (a.contains("content")) {
          return ToolWriteFile(path, JsonValue(a, "content", ""));
        }
        std::vector<FileEdit> edits;
        for (const json& item : a["edits"]) {
          edits.push_back({JsonValue(item, "old", ""),
                           JsonValue(item, "new", ""),
                           JsonValue(item, "replace_all", false)});
        }
        return ToolEditFile(path, edits);
      }));
  edit.mutating = true;
  edit.capabilities = Capability(ToolCapability::kMutate);
  edit.available_in_lean = false;
  edit.validate = [](const json& a) {
    bool content = a.contains("content");
    bool edits = a.contains("edits");
    if (content == edits) {
      return std::string("error: supply either content or edits");
    }
    return std::string();
  };
  edit.summary = [](const json& a) {
    std::string path = JsonValue(a, "path", "");
    if (a.contains("content")) {
      return path + " (" +
             FmtBytes(static_cast<int64_t>(
                 JsonValue(a, "content", std::string()).size())) +
             ")";
    }
    size_t count = 0;
    auto additional = a.find("edits");
    if (additional != a.end() && additional->is_array()) {
      count = additional->size();
    }
    return path + " (" + std::to_string(count) +
           (count == 1 ? " edit)" : " edits)");
  };

  Tool& grep = path_tool(MakeTool(
      "grep",
      "Locate file paths or matching content with a regex under an optional "
      "path and glob. Use mode=files to match paths and read_path afterward.",
      schema(R"json({"type":"object","properties":{
                    "pattern":{"type":"string","minLength":1},"path":{"type":"string"},
                    "glob":{"type":"string"},
                    "mode":{"type":"string","enum":["content","files"]},
                    "context":{"type":"integer","minimum":0,"maximum":10,
                      "description":"surrounding content lines"}},"required":["pattern"]})json"),
      [&supervisor](const json& a, const ToolContext& context) {
        return ToolGrep(supervisor, JsonValue(a, "pattern", ""),
                        JsonValue(a, "path", "."), JsonValue(a, "glob", ""),
                        JsonValue(a, "context", int64_t{0}), context,
                        JsonValue(a, "mode", "content") == "files");
      }));
  grep.parallel_safe = true;  // read-only, like read_path
  grep.capabilities = Capability(ToolCapability::kInspect);
  grep.summary = [](const json& a) {
    std::string mode = JsonValue(a, "mode", "content");
    return (mode == "files" ? "files /" : "/") + JsonValue(a, "pattern", "") +
           "/ in " + JsonValue(a, "path", ".");
  };

  if (inline_images) {
    Tool& show_image = path_tool(
        MakeTool("show_image",
                 "Display a local image using the native terminal protocol.",
                 schema(R"json({"type":"object","properties":{
                              "path":{"type":"string"}},"required":["path"]})json"),
                 [](const json& a, const ToolContext&) {
                   return ToolShowImage(JsonValue(a, "path", ""));
                 }));
    show_image.capabilities = Capability(ToolCapability::kInspect);
    show_image.serial_media = true;
    show_image.replay_image = true;
  }

  // read_path only handles text. This puts the bytes themselves in front of
  // the model, so it can read what it cannot parse.
  Tool& attach = path_tool(MakeTool(
      "attach",
      "Add an image/document to model context when read_path cannot parse it.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"}},"required":["path"]})json"),
      [](const json& a, const ToolContext& context) {
        return Attachments().Add(JsonValue(a, "path", ""),
                                 context.image_input_available,
                                 context.image_fallback_available);
      }));
  attach.parallel_safe = true;
  attach.capabilities = Capability(ToolCapability::kInspect);
  attach.max_calls_per_turn = 4;  // each one rides on the next request

  Tool& run = AddTool(
      tools,
      MakeTool("run",
               "Execute a non-privileged build, test, or shell command in cwd "
               "(bash default; omit cd; no sudo). Set tty=true only when the "
               "process needs interactive stdin. Do not use it for file "
               "search, reading, or editing when a dedicated tool exists. "
               "Use a project's existing Python runner such as uv run or "
               "pytest. Detach only for a persistent terminal that may outlive "
               "the current session.",
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
                     JsonValue(a, "max_output_chars", int64_t{0}));
               }));
  run.mutating = true;
  run.capabilities = Capability(ToolCapability::kExecute) |
                     Capability(ToolCapability::kMutate);
  run.validate = [](const json& a) {
    std::string error = RunCommandPolicyError(JsonValue(a, "command", ""));
    if (!error.empty()) return error;
    int64_t yield_ms = JsonValue(a, "yield_ms", int64_t{0});
    if (yield_ms > 0 && yield_ms < 250) {
      return std::string("error: yield_ms must be 0 or at least 250");
    }
    return std::string();
  };
  run.summary = [](const json& a) { return JsonValue(a, "command", ""); };
  run.timeout_s = 0;  // bounded by the turn; Escape remains responsive
  // Each call owns its process group and log, so independent commands
  // (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;
  run.verbatim_label = true;
  run.command_policy = true;

  Tool& python = AddTool(
      tools,
      MakeTool(
          "scratch",
          "Run a one-off Python script when shell is insufficient, never for "
          "requested project code. Writes or replaces one persistent script "
          "under .uagent/scratch and runs it under isolated uv.",
          schema(
              R"json({"type":"object","additionalProperties":false,"properties":{
                    "path":{"type":"string","minLength":1,
                      "description":"stable relative .py path; reuse it during the task"},
                    "code":{"type":["string","null"],"minLength":1,"maxLength":131072,
                      "description":"script body, without PEP 723 metadata; null reruns the file unchanged"},
                    "packages":{"type":["array","null"],"items":{"type":"string","minLength":1,"maxLength":256},"maxItems":12,
                      "description":"PEP 508 dependencies with code ([] for stdlib); null when rerunning"}},
                    "required":["path","code","packages"]})json"),
          [&supervisor, workspace](const json& a, const ToolContext& context) {
            return ToolRunPython(
                supervisor, workspace, JsonValue(a, "path", ""),
                JsonValue(a, "code", json(nullptr)),
                JsonValue(a, "packages", json(nullptr)), context);
          }));
  python.mutating = true;
  python.capabilities = Capability(ToolCapability::kExecute) |
                        Capability(ToolCapability::kMutate);
  python.summary = [](const json& a) {
    std::string path = JsonValue(a, "path", "");
    return a.contains("code") && a["code"].is_string()
               ? "write/replace " + path + " → execute"
               : "execute " + path;
  };
  python.stable_argument = "path";
  python.timeout_s = 0;  // bounded by the turn; no model-driven polling

  // One tool for looking at running work: list them, drain one, write to one,
  // or block until they finish. These were four tools whose only real
  // difference was which optional argument was supplied, and their schemas
  // cost more than the whole file-editing surface.
  Tool& activity = AddTool(
      tools,
      MakeTool(
          "activity",
          "Inspect or drive activities. Omit id to list them, or with wait_ms "
          "to block until any/all finish. With id, returns that activity's new "
          "output; chars writes to it first (\\u0003 interrupts, empty polls), "
          "and rows+cols resize a PTY. until returns early on a readiness "
          "marker. Completion never starts a model turn.",
          schema(
              R"json({"type":"object","additionalProperties":false,"properties":{
                    "id":{"type":"integer","minimum":1,"maximum":2147483647},
                    "chars":{"type":"string","maxLength":65536},
                    "wait_ms":{"type":"integer","minimum":0,"maximum":300000},
                    "until":{"type":"string","maxLength":256},
                    "mode":{"type":"string","enum":["any","all"]},
                    "rows":{"type":"integer","minimum":1,"maximum":1000},
                    "cols":{"type":"integer","minimum":1,"maximum":1000},
                    "max_output_chars":{"type":"integer","minimum":256,"maximum":65536}}})json"),
          [&supervisor](const json& a, const ToolContext& context) {
            int64_t id = JsonValue(a, "id", int64_t{0});
            int64_t cap = JsonValue(a, "max_output_chars", int64_t{0});
            bool writing = a.contains("chars") || a.contains("rows");
            // Writes settle quickly; a blocking wait is asked for explicitly
            // and a bare drain returns what is already buffered.
            int64_t wait_ms =
                JsonValue(a, "wait_ms", writing ? int64_t{250} : int64_t{0});
            if (writing) {
              return ToolActivityInput(
                  supervisor, id, JsonValue(a, "chars", ""), wait_ms, context,
                  JsonValue(a, "rows", int64_t{0}),
                  JsonValue(a, "cols", int64_t{0}), cap);
            }
            if (id <= 0 && wait_ms > 0) {
              return ToolActivityWait(supervisor, {},
                                      JsonValue(a, "mode", "any"), wait_ms,
                                      context, cap);
            }
            return ToolActivityOutput(supervisor, id, wait_ms,
                                      JsonValue(a, "until", ""), context, cap);
          }));
  activity.parallel_safe = true;
  // Reading and writing share one tool, so the union gates exposure and the
  // per-call predicate gates approval, as the memory tool does.
  activity.capabilities = Capability(ToolCapability::kInspect) |
                          Capability(ToolCapability::kExecute) |
                          Capability(ToolCapability::kMutate);
  activity.mutates = [](const json& a) {
    return a.contains("chars") || a.contains("rows") || a.contains("cols");
  };
  activity.result_chars = kActivityResultChars;
  activity.blocking_wait_default_ms = 0;
  activity.visibility = Tool::Visibility::kDetachedTerminal;
  activity.validate = [](const json& a) {
    if (a.contains("until") && (!a.contains("id") || !a.contains("wait_ms"))) {
      return std::string("error: until requires id and wait_ms");
    }
    if (a.contains("rows") != a.contains("cols")) {
      return std::string("error: rows and cols must be supplied together");
    }
    if ((a.contains("chars") || a.contains("rows")) &&
        JsonValue(a, "id", int64_t{0}) <= 0) {
      return std::string("error: writing requires id");
    }
    return std::string();
  };
  activity.summary = [](const json& a) {
    int64_t id = JsonValue(a, "id", int64_t{0});
    std::string target = id > 0
                             ? "activity " + std::to_string(id)
                             : JsonValue(a, "mode", "any") + " · all current";
    if (a.contains("chars")) {
      return target + " · " +
             FmtBytes(static_cast<int64_t>(JsonValue(a, "chars", "").size()));
    }
    int64_t wait_ms = JsonValue(a, "wait_ms", int64_t{0});
    return wait_ms > 0 ? target + " · " + FmtDuration(wait_ms / 1000.0)
                       : target;
  };

  Tool& activity_stop = AddTool(
      tools,
      MakeTool("activity_stop",
               "Stop an activity's complete process group and clean its log.",
               schema(R"json({"type":"object","properties":{
                    "id":{"type":"integer","minimum":1,"maximum":2147483647}},
                    "required":["id"]})json"),
               [&supervisor](const json& a, const ToolContext&) {
                 return ToolActivityStop(supervisor,
                                         JsonValue(a, "id", int64_t{0}));
               }));
  activity_stop.mutating = true;
  activity_stop.capabilities = Capability(ToolCapability::kExecute) |
                               Capability(ToolCapability::kMutate);
  activity_stop.visibility = Tool::Visibility::kDetachedTerminal;
  activity_stop.summary = [](const json& a) {
    return "activity " + std::to_string(JsonValue(a, "id", int64_t{0}));
  };

  json memory_schema = schema(R"json({"type":"object","properties":{
                    "action":{"type":"string","enum":["get","set","forget","list","search"]},
                    "key":{"type":"string",
                      "description":"exact project/<name> or global/<name> key; codex/<name> and claude/<name> are read-only; search text for search; omit for list"},
                    "content":{"type":"string",
                      "description":"durable lesson; required only for set"}},
                    "required":["action"]})json");
  bool automatic_extraction = !EnvStr("UAGENT_INTERNAL_MEMORY_SOURCE").empty();
  bool automatic_write = false;
  Tool& memory = AddTool(
      tools,
      MakeTool(
          "memory",
          "List or search memory when the startup index is insufficient; get "
          "a body only when relevant. Set or forget only when the user asks, "
          "except that the dedicated background extractor may set one native "
          "memory. Never save task progress, guesses, secrets, commands, or "
          "permissions. Codex and Claude memories are read-only.",
          std::move(memory_schema),
          [automatic_extraction, automatic_write](const json& a,
                                                  const ToolContext&) mutable {
            std::string action = JsonValue(a, "action", "");
            if (automatic_extraction && action == "forget") {
              return ToolFailure(ToolErrorCode::kPermissionDenied,
                                 "error: background extraction cannot forget "
                                 "memory");
            }
            if (automatic_extraction && automatic_write && action == "set") {
              return ToolFailure(
                  ToolErrorCode::kLimitExceeded,
                  "error: background extraction already wrote one memory");
            }
            std::optional<std::string> content;
            if (a.contains("content") && a["content"].is_string()) {
              content = a["content"].get<std::string>();
            }
            ToolResult result =
                ToolMemoryAction(action, JsonValue(a, "key", ""), content);
            if (automatic_extraction && action == "set" && result.Ok()) {
              automatic_write = true;
            }
            return result;
          }));
  memory.mutates = [](const json& a) {
    std::string action = JsonValue(a, "action", "");
    return action == "set" || action == "forget";
  };
  memory.capabilities = Capability(ToolCapability::kInspect) |
                        Capability(ToolCapability::kMutate);
  memory.available_in_lean = false;
  memory.memory_store = true;
  memory.retain_output = true;
  memory.summary = [](const json& a) {
    return JsonValue(a, "action", "") + " " + JsonValue(a, "key", "");
  };

  return tools;
}

}  // namespace uagent
