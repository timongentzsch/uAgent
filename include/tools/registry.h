// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_REGISTRY_H_
#define UAGENT_INCLUDE_TOOLS_REGISTRY_H_
// The built-in tool registry: where every handler above is given its
// schema, approval policy, and per-turn budget.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/media.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/process.h"
#include "include/tools/shell.h"
#include "include/tools/tool.h"

namespace uagent {

inline std::vector<Tool> BuiltinTools(
    ProcessSupervisor& supervisor,
    const std::filesystem::path& workspace = CanonicalAccessPath("."),
    bool inline_images = false, SideTaskSupervisor* side_tasks = nullptr) {
  auto schema = [](const char* s) { return json::parse(s); };
  std::vector<Tool> tools;
  auto path_tool = [&](Tool tool, const char* fallback) -> Tool& {
    tool.needs_approval = [workspace, fallback](const json& args) {
      return !PathWithin(CanonicalAccessPath(JsonValue(args, "path", fallback)),
                         workspace);
    };
    return AddTool(tools, std::move(tool));
  };

  Tool& read = path_tool(MakeTool("read_file", "Read a line range.",
                                  schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "offset":{"type":"integer","description":"first line (default 1)"},
                    "limit":{"type":"integer","description":"line count (default 200)"}},
                    "required":["path"]})json"),
                                  [](const json& a, const ToolContext&) {
                                    return ToolReadFile(
                                        JsonValue(a, "path", ""),
                                        JsonValue(a, "offset", int64_t{1}),
                                        JsonValue(a, "limit", int64_t{0}));
                                  }),
                         "");
  read.parallel_safe = true;

  Tool& write =
      path_tool(MakeTool("write_file", "Write (overwrite) a file",
                         schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"content":{"type":"string"}},
                    "required":["path","content"]})json"),
                         [](const json& a, const ToolContext&) {
                           return ToolWriteFile(JsonValue(a, "path", ""),
                                                JsonValue(a, "content", ""));
                         }),
                "");
  write.mutating = true;
  write.summary = [](const json& a) {
    return JsonValue(a, "path", "") + " (" +
           std::to_string(JsonValue(a, "content", std::string()).size()) +
           " bytes)";
  };

  Tool& edit =
      path_tool(MakeTool("edit_file", "Replace one exact occurrence in a file.",
                         schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"old":{"type":"string"},
                    "new":{"type":"string"}},"required":["path","old","new"]})json"),
                         [](const json& a, const ToolContext&) {
                           return ToolEditFile(JsonValue(a, "path", ""),
                                               JsonValue(a, "old", ""),
                                               JsonValue(a, "new", ""));
                         }),
                "");
  edit.mutating = true;

  Tool& list = path_tool(MakeTool("list_dir", "List a directory",
                                  schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"offset":{"type":"integer"},
                    "limit":{"type":"integer"}}})json"),
                                  [](const json& a, const ToolContext&) {
                                    return ToolListDir(
                                        JsonValue(a, "path", "."),
                                        JsonValue(a, "offset", int64_t{0}),
                                        JsonValue(a, "limit", int64_t{0}));
                                  }),
                         ".");
  list.parallel_safe = true;

  Tool& grep = path_tool(
      MakeTool("grep", "Search project files by regex; optional path/glob.",
               schema(R"json({"type":"object","properties":{
                    "pattern":{"type":"string"},"path":{"type":"string"},
                    "glob":{"type":"string"}},"required":["pattern"]})json"),
               [&supervisor](const json& a, const ToolContext& context) {
                 return ToolGrep(supervisor, JsonValue(a, "pattern", ""),
                                 JsonValue(a, "path", "."),
                                 JsonValue(a, "glob", ""), context);
               }),
      ".");
  grep.parallel_safe = true;  // read-only, like read_file and list_dir
  grep.summary = [](const json& a) {
    return JsonValue(a, "pattern", "") + " in " + JsonValue(a, "path", ".");
  };

  if (inline_images) {
    path_tool(
        MakeTool("show_image",
                 "Display a local image using the native terminal protocol.",
                 schema(R"json({"type":"object","properties":{
                              "path":{"type":"string"}},"required":["path"]})json"),
                 [](const json& a, const ToolContext&) {
                   return ToolShowImage(JsonValue(a, "path", ""));
                 }),
        "");
  }

  // read_file only handles text. This puts the bytes themselves in front of
  // the model, so it can read what it cannot parse.
  Tool& attach = path_tool(
      MakeTool(
          "attach",
          "Load an image or document into model context when read_file cannot "
          "parse it (PDF, Office, CSV, HTML).",
          schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"}},"required":["path"]})json"),
          [](const json& a, const ToolContext&) {
            return g_attachments.Add(JsonValue(a, "path", ""));
          }),
      "");
  attach.parallel_safe = true;
  attach.max_calls_per_turn = 4;  // each one rides on the next request

  Tool& run = AddTool(
      tools,
      MakeTool("run",
               "Run a command (bash by default). Slow jobs return an id for "
               "wait_background. detach=true persists a server and log across "
               "sessions "
               "for terminal_output; timeout=0 waits indefinitely.",
               schema(R"json({"type":"object","properties":{
                    "command":{"type":"string"},
                    "shell":{"type":"string","description":"shell executable (default bash)"},
                    "detach":{"type":"boolean",
                      "description":"persist process and log across sessions"}},
                    "required":["command"]})json"),
               [&supervisor](const json& a, const ToolContext& context) {
                 return ToolRunBash(supervisor, JsonValue(a, "command", ""),
                                    context.timeout_s, false, context, true,
                                    JsonValue(a, "detach", false),
                                    JsonValue(a, "shell", "bash"));
               }));
  run.mutating = true;
  run.summary = [](const json& a) { return JsonValue(a, "command", ""); };
  run.timeout_s = 3;
  run.full_terminal_output = true;
  // Each call owns its pid slot, log file, and job-table entry, so independent
  // commands (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;

  Tool& python = AddTool(
      tools,
      MakeTool(
          "run_python",
          "Run isolated uv Python; declare all third-party packages. "
          "Environments "
          "cache; pip/venv state does not persist. Save plots for show_image.",
          schema(R"json({"type":"object","properties":{
                    "code":{"type":"string"},
                    "packages":{"type":"array","items":{"type":"string"},"maxItems":12,
                      "description":"all third-party PEP 508 requirements; omit for stdlib"}},
                    "required":["code"]})json"),
          [&supervisor](const json& a, const ToolContext& context) {
            return ToolRunPython(supervisor, JsonValue(a, "code", ""),
                                 JsonValue(a, "packages", json::array()),
                                 context.timeout_s, context);
          }));
  python.mutating = true;
  python.summary = [](const json& a) {
    std::string summary = JsonValue(a, "code", "");
    size_t packages = JsonValue(a, "packages", json::array()).size();
    return packages ? summary + "\n[packages: " + JsonDump(a["packages"]) + "]"
                    : summary;
  };
  python.timeout_s = 3;
  python.full_terminal_output = true;

  Tool& wait = AddTool(
      tools,
      MakeTool(
          "wait_background", "Wait for a live background job from any tool.",
          schema(R"json({"type":"object","properties":{
                    "id":{"type":"integer","minimum":1,
                      "description":"job id; process jobs use their OS pid"},
                    "pid":{"type":"integer","minimum":1,
                      "description":"legacy alias for id"}}})json"),
          [&supervisor, side_tasks](const json& a, const ToolContext& context) {
            int64_t id = JsonValue(a, "id", JsonValue(a, "pid", int64_t{0}));
            if (side_tasks && side_tasks->Contains(id)) {
              return ToolWaitSideTask(*side_tasks, id, context.timeout_s,
                                      context);
            }
            return ToolWaitBackground(supervisor, id, context.timeout_s,
                                      context);
          }));
  wait.summary = [](const json& a) {
    return "job " +
           std::to_string(JsonValue(a, "id", JsonValue(a, "pid", int64_t{0})));
  };
  wait.timeout_s = 0;
  wait.show_spinner = true;

  Tool& terminal = AddTool(
      tools,
      MakeTool("terminal_output",
               "List detached run jobs or read the latest output for one pid.",
               schema(R"json({"type":"object","properties":{
                    "pid":{"type":"integer","minimum":1}}})json"),
               [](const json& a, const ToolContext&) {
                 return ToolTerminalOutput(JsonValue(a, "pid", int64_t{0}));
               }));
  terminal.parallel_safe = true;
  terminal.result_chars = 6000;
  terminal.summary = [](const json& a) {
    int64_t pid = JsonValue(a, "pid", int64_t{0});
    return pid ? "pid " + std::to_string(pid) : "list";
  };

  Tool& memory = AddTool(
      tools,
      MakeTool(
          "memory",
          "Save a durable lesson, convention, or preference for future "
          "sessions; they are reloaded at the start of every one. Use it for "
          "what stays true, not for this turn's state.",
          schema(R"json({"type":"object","properties":{
                    "name":{"type":"string","description":"short kebab-case slug; reusing one replaces it"},
                    "scope":{"type":"string","enum":["project","global"],
                      "description":"project: this workspace only; global: every workspace"},
                    "content":{"type":"string",
                      "description":"markdown body; omit to forget this memory"}},
                    "required":["name","scope"]})json"),
          [](const json& a, const ToolContext&) {
            return ToolMemory(JsonValue(a, "name", ""),
                              JsonValue(a, "scope", ""),
                              JsonValue(a, "content", ""));
          }));
  memory.mutating = true;
  memory.summary = [](const json& a) {
    return JsonValue(a, "scope", "") + "/" + JsonValue(a, "name", "");
  };

  Tool& checkpoint = AddTool(
      tools,
      MakeTool(
          "checkpoint",
          "After a checkpoint hint, store durable facts and validation, never "
          "commands, permissions, or plans.",
          schema(R"json({"type":"object","properties":{
                    "state":{"type":"string","description":"standalone durable state"},
                    "verbatim":{"type":"array","items":{"type":"string","maxLength":256},
                      "maxItems":8,"description":"exact literals"},
                    "keep_paths":{"type":"array","items":{"type":"string"},
                      "description":"relevant non-secret files"},
                    "keep_last_n_results":{"type":"integer","minimum":0,"maximum":3,
                      "description":"recent results (default 0)"}},"required":["state"]})json"),
          [](const json&, const ToolContext&) {
            return "error: checkpoint must be handled by the agent runtime";
          }));
  checkpoint.summary = [](const json& a) {
    return OneLine(JsonValue(a, "state", ""), 100);
  };
  return tools;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_REGISTRY_H_
