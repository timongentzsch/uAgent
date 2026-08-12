// Copyright 2026 Timon Gentzsch

#include "include/tools/registry.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/media.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/tools/path_policy.h"
#include "include/tools/shell.h"

namespace uagent {

std::vector<Tool> BuiltinTools(ProcessSupervisor& supervisor,
                               const std::filesystem::path& workspace,
                               bool inline_images) {
  auto schema = [](const char* s) { return json::parse(s); };
  std::vector<Tool> tools;
  auto path_tool = [&](Tool tool) -> Tool& {
    tool.needs_approval = [workspace](const json& args) {
      return PathApprovalRequired(JsonValue(args, "path", ""), workspace);
    };
    return AddTool(tools, std::move(tool));
  };

  Tool& read = path_tool(MakeTool(
      "read_file",
      "Read a known text file or useful line range. Results remain in context; "
      "do not reread an unchanged range. Reread after edits or external "
      "changes when exact current text matters. Use grep when the file or "
      "symbol is unknown; batch independent known files.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "offset":{"type":"integer","description":"first line (default 1)"},
                    "limit":{"type":"integer","description":"line count (default 1000)"}},
                    "required":["path"]})json"),
      [](const json& a, const ToolContext&) {
        return ToolReadFile(JsonValue(a, "path", ""),
                            JsonValue(a, "offset", int64_t{1}),
                            JsonValue(a, "limit", int64_t{0}));
      }));
  read.parallel_safe = true;
  read.capabilities = Capability(ToolCapability::kInspect);
  read.dedupe_output = true;
  // Reads get a larger, contiguous window than logs and remote output. This
  // avoids paying another model round merely to continue an ordinary source
  // file while keeping every other tool on the global result cap.
  read.result_chars = ReadFileResultChars();

  Tool& write =
      path_tool(MakeTool("write_file",
                         "Create a file or replace its complete contents. Use "
                         "edit_file for localized changes.",
                         schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"content":{"type":"string"}},
                    "required":["path","content"]})json"),
                         [](const json& a, const ToolContext&) {
                           return ToolWriteFile(JsonValue(a, "path", ""),
                                                JsonValue(a, "content", ""));
                         }));
  write.mutating = true;
  write.capabilities = Capability(ToolCapability::kMutate);
  write.available_in_lean = false;
  write.summary = [](const json& a) {
    return JsonValue(a, "path", "") + " (" +
           std::to_string(JsonValue(a, "content", std::string()).size()) +
           " bytes)";
  };

  Tool& edit = path_tool(MakeTool(
      "edit_file",
      "Modify an existing text file by exact search/replace. Batch related "
      "edits; all apply atomically in order.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "edits":{"type":"array","minItems":1,"maxItems":64,
                      "description":"one or more exact replacements in order",
                      "items":{"type":"object","properties":{
                        "old":{"type":"string"},"new":{"type":"string"},
                        "replace_all":{"type":"boolean"}},
                        "required":["old","new"],"additionalProperties":false}}},
                    "required":["path","edits"]})json"),
      [](const json& a, const ToolContext&) {
        std::vector<FileEdit> edits;
        for (const json& item : a["edits"]) {
          edits.push_back({JsonValue(item, "old", ""),
                           JsonValue(item, "new", ""),
                           JsonValue(item, "replace_all", false)});
        }
        return ToolEditFile(JsonValue(a, "path", ""), edits);
      }));
  edit.mutating = true;
  edit.capabilities = Capability(ToolCapability::kMutate);
  edit.available_in_lean = false;
  edit.summary = [](const json& a) {
    size_t count = 0;
    auto additional = a.find("edits");
    if (additional != a.end() && additional->is_array()) {
      count = additional->size();
    }
    return JsonValue(a, "path", "") + " (" + std::to_string(count) +
           (count == 1 ? " edit)" : " edits)");
  };

  Tool& list = path_tool(MakeTool(
      "list_dir",
      "Inspect one directory. Use grep for recursive content search and "
      "read_file for a known file.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"offset":{"type":"integer"},
                    "limit":{"type":"integer"}}})json"),
      [workspace](const json& a, const ToolContext&) {
        std::string path = JsonValue(a, "path", ".");
        bool preview = !PathApprovalRequired(path, workspace);
        return ToolListDir(path, JsonValue(a, "offset", int64_t{0}),
                           JsonValue(a, "limit", int64_t{0}), preview);
      }));
  list.parallel_safe = true;
  list.capabilities = Capability(ToolCapability::kInspect);

  Tool& grep = path_tool(MakeTool(
      "grep",
      "Locate file paths or matching content with a regex under an optional "
      "path and glob. Use mode=files to match paths and read_file afterward.",
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
  grep.parallel_safe = true;  // read-only, like read_file and list_dir
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
  }

  // read_file only handles text. This puts the bytes themselves in front of
  // the model, so it can read what it cannot parse.
  Tool& attach = path_tool(MakeTool(
      "attach",
      "Add an image/document to model context when read_file cannot parse it.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"}},"required":["path"]})json"),
      [](const json& a, const ToolContext&) {
        return Attachments().Add(JsonValue(a, "path", ""));
      }));
  attach.parallel_safe = true;
  attach.capabilities = Capability(ToolCapability::kInspect);
  attach.max_calls_per_turn = 4;  // each one rides on the next request

  Tool& run = AddTool(
      tools,
      MakeTool("run",
               "Execute a non-privileged build, test, or shell command in cwd "
               "(bash default; omit cd; no stdin or sudo). Do not use it for "
               "file search, reading, or editing when a dedicated tool exists. "
               "Use a project's existing Python runner such as uv run or "
               "pytest. Detach only for a persistent terminal that may outlive "
               "the current session.",
               schema(R"json({"type":"object","properties":{
                    "command":{"type":"string"},
                    "shell":{"type":"string","description":"default bash"},
                    "detach":{"type":"boolean",
                      "description":"persist terminal and log"}},
                    "required":["command"]})json"),
               [&supervisor](const json& a, const ToolContext& context) {
                 return ToolRunApprovedShell(
                     supervisor, JsonValue(a, "command", ""), context,
                     JsonValue(a, "detach", false),
                     JsonValue(a, "shell", "bash"));
               }));
  run.mutating = true;
  run.capabilities = Capability(ToolCapability::kExecute) |
                     Capability(ToolCapability::kMutate);
  run.validate = [](const json& a) {
    return RunCommandPolicyError(JsonValue(a, "command", ""));
  };
  run.summary = [](const json& a) { return JsonValue(a, "command", ""); };
  run.timeout_s = 0;  // bounded by the turn; Escape remains responsive
  // Each call owns its process group and log, so independent commands
  // (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;

  Tool& python = AddTool(
      tools,
      MakeTool(
          "run_python",
          "Run a one-off Python scratch script when shell is insufficient. "
          "Never use this for requested project implementation: edit project "
          "files and use the project runner. Code writes or replaces one "
          "stable scratch script and executes it; code=null executes the "
          "existing file unchanged after edit_file changes or when only "
          "imported files changed. Never resend unchanged code. Scripts "
          "persist in "
          ".uagent/scratch; dependencies use PEP 723 and isolated uv.",
          schema(
              R"json({"type":"object","additionalProperties":false,"properties":{
                    "path":{"type":"string","minLength":1,
                      "description":"One stable relative .py path under .uagent/scratch; reuse it during the task."},
                    "code":{"type":["string","null"],"minLength":1,"maxLength":131072,
                      "description":"Script body when creating/replacing; null reruns the existing file after read_file/edit_file changes. Do not include PEP 723 metadata."},
                    "packages":{"type":["array","null"],"items":{"type":"string","minLength":1,"maxLength":256},"maxItems":12,
                      "description":"PEP 508 dependencies when code is provided ([] for stdlib); null when rerunning."}},
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

  Tool& activity_output = AddTool(
      tools, MakeTool("activity_output",
                      "List activities or read one bounded log. Set wait_ms "
                      "for new output; add until for a readiness marker.",
                      schema(R"json({"type":"object","properties":{
                    "id":{"type":"integer","minimum":1,"maximum":2147483647},
                    "wait_ms":{"type":"integer","minimum":1000,"maximum":300000},
                    "until":{"type":"string","maxLength":256}}})json"),
                      [&supervisor](const json& a, const ToolContext& context) {
                        return ToolActivityOutput(
                            supervisor, JsonValue(a, "id", int64_t{0}),
                            JsonValue(a, "wait_ms", int64_t{0}),
                            JsonValue(a, "until", ""), context);
                      }));
  activity_output.parallel_safe = true;
  activity_output.capabilities = Capability(ToolCapability::kInspect);
  activity_output.result_chars = 6000;
  activity_output.visibility = Tool::Visibility::kDetachedTerminal;
  activity_output.validate = [](const json& a) {
    if (a.contains("until") && (!a.contains("id") || !a.contains("wait_ms"))) {
      return std::string("error: until requires id and wait_ms");
    }
    return std::string();
  };
  activity_output.summary = [](const json& a) {
    int64_t id = JsonValue(a, "id", int64_t{0});
    int64_t wait_ms = JsonValue(a, "wait_ms", int64_t{0});
    std::string summary = id ? "activity " + std::to_string(id) : "list";
    if (wait_ms > 0) summary += " · wait " + std::to_string(wait_ms) + "ms";
    std::string until = JsonValue(a, "until", "");
    if (!until.empty()) summary += " · until " + FirstLine(until);
    return summary;
  };

  Tool& activity_wait = AddTool(
      tools,
      MakeTool("activity_wait",
               "Block for any/all selected command or task activities only "
               "when their result is required for the next step. Background "
               "results otherwise arrive automatically. Omit ids for all "
               "current non-detached activities.",
               schema(R"json({"type":"object","properties":{
                    "ids":{"type":"array","maxItems":20,
                      "items":{"type":"integer","minimum":1,"maximum":2147483647}},
                    "mode":{"type":"string","enum":["any","all"]},
                    "wait_ms":{"type":"integer","minimum":1000,"maximum":300000}}})json"),
               [&supervisor](const json& a, const ToolContext& context) {
                 std::vector<int64_t> ids;
                 if (a.contains("ids")) {
                   ids.reserve(a["ids"].size());
                   for (const json& id : a["ids"]) {
                     ids.push_back(id.get<int64_t>());
                   }
                 }
                 return ToolActivityWait(
                     supervisor, ids, JsonValue(a, "mode", "any"),
                     JsonValue(a, "wait_ms", int64_t{30000}), context);
               }));
  activity_wait.parallel_safe = true;
  activity_wait.capabilities = Capability(ToolCapability::kInspect);
  activity_wait.result_chars = 6000;
  activity_wait.timeout_s = 0;
  activity_wait.visibility = Tool::Visibility::kDetachedTerminal;
  activity_wait.summary = [](const json& a) {
    size_t count = a.contains("ids") ? a["ids"].size() : 0;
    return JsonValue(a, "mode", "any") + " · " +
           (count ? std::to_string(count) : std::string("all current")) +
           " · " + std::to_string(JsonValue(a, "wait_ms", int64_t{30000})) +
           "ms";
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
  Tool& memory = AddTool(
      tools,
      MakeTool(
          "memory",
          "List or search memory when the startup index is insufficient; get "
          "a body only when relevant. Set or forget only when the user asks, "
          "except that the dedicated background extractor may set one native "
          "memory. Never save task progress, guesses, secrets, commands, or "
          "permissions. Codex and Claude memories are read-only.",
          std::move(memory_schema), [](const json& a, const ToolContext&) {
            std::optional<std::string> content;
            if (a.contains("content") && a["content"].is_string()) {
              content = a["content"].get<std::string>();
            }
            return ToolMemoryAction(JsonValue(a, "action", ""),
                                    JsonValue(a, "key", ""), content);
          }));
  memory.mutates = [](const json& a) {
    std::string action = JsonValue(a, "action", "");
    return action == "set" || action == "forget";
  };
  memory.capabilities = Capability(ToolCapability::kInspect) |
                        Capability(ToolCapability::kMutate);
  memory.available_in_lean = false;
  memory.retain_output = true;
  memory.summary = [](const json& a) {
    return JsonValue(a, "action", "") + " " + JsonValue(a, "key", "");
  };

  return tools;
}

}  // namespace uagent
