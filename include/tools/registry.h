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
    bool inline_images = false) {
  auto schema = [](const char* s) { return json::parse(s); };
  std::vector<Tool> tools;
  auto path_tool = [&](Tool tool) -> Tool& {
    tool.needs_approval = [workspace](const json& args) {
      return PathApprovalRequired(JsonValue(args, "path", ""), workspace);
    };
    return AddTool(tools, std::move(tool));
  };

  Tool& read = path_tool(MakeTool("read_file", "Read a line range.",
                                  schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "offset":{"type":"integer","description":"first line (default 1)"},
                    "limit":{"type":"integer","description":"line count (default 1000)"}},
                    "required":["path"]})json"),
                                  [](const json& a, const ToolContext&) {
                                    return ToolReadFile(
                                        JsonValue(a, "path", ""),
                                        JsonValue(a, "offset", int64_t{1}),
                                        JsonValue(a, "limit", int64_t{0}));
                                  }));
  read.parallel_safe = true;
  // Reads get a larger, contiguous window than logs and remote output. This
  // avoids paying another model round merely to continue an ordinary source
  // file while keeping every other tool on the global result cap.
  read.result_chars = ReadFileResultChars();

  Tool& write =
      path_tool(MakeTool("write_file", "Overwrite a file.",
                         schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"content":{"type":"string"}},
                    "required":["path","content"]})json"),
                         [](const json& a, const ToolContext&) {
                           return ToolWriteFile(JsonValue(a, "path", ""),
                                                JsonValue(a, "content", ""));
                         }));
  write.mutating = true;
  write.available_in_lean = false;
  write.summary = [](const json& a) {
    return JsonValue(a, "path", "") + " (" +
           std::to_string(JsonValue(a, "content", std::string()).size()) +
           " bytes)";
  };

  Tool& edit = path_tool(MakeTool(
      "edit_file", "Apply exact edits atomically in order.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"old":{"type":"string"},
                    "new":{"type":"string"},
                    "replace_all":{"type":"boolean","description":"replace all matches"},
                    "edits":{"type":"array","maxItems":63,
                      "description":"additional edits in order",
                      "items":{"type":"object","properties":{
                        "old":{"type":"string"},"new":{"type":"string"},
                        "replace_all":{"type":"boolean"}},
                        "required":["old","new"]}}},
                    "required":["path","old","new"]})json"),
      [](const json& a, const ToolContext&) {
        std::vector<FileEdit> edits{{
            JsonValue(a, "old", ""),
            JsonValue(a, "new", ""),
            JsonValue(a, "replace_all", false),
        }};
        auto additional = a.find("edits");
        if (additional != a.end()) {
          if (!additional->is_array()) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: `edits` must be an array");
          }
          if (additional->size() + 1 > kMaxFileEdits) {
            return ToolFailure(ToolErrorCode::kLimitExceeded,
                               "error: `edit_file` is limited to " +
                                   std::to_string(kMaxFileEdits) +
                                   " edits per call");
          }
          for (const json& item : *additional) {
            if (!item.is_object() || !item.contains("old") ||
                !item["old"].is_string() || !item.contains("new") ||
                !item["new"].is_string() ||
                (item.contains("replace_all") &&
                 !item["replace_all"].is_boolean())) {
              return ToolFailure(ToolErrorCode::kInvalidArguments,
                                 "error: each `edits` entry requires "
                                 "string `old`/`new` and optional boolean "
                                 "`replace_all`");
            }
            edits.push_back({
                JsonValue(item, "old", ""),
                JsonValue(item, "new", ""),
                JsonValue(item, "replace_all", false),
            });
          }
        }
        return ToolEditFile(JsonValue(a, "path", ""), edits);
      }));
  edit.mutating = true;
  edit.available_in_lean = false;
  edit.summary = [](const json& a) {
    size_t count = 1;
    auto additional = a.find("edits");
    if (additional != a.end() && additional->is_array()) {
      count += additional->size();
    }
    return JsonValue(a, "path", "") + " (" + std::to_string(count) +
           (count == 1 ? " edit)" : " edits)");
  };

  Tool& list = path_tool(MakeTool(
      "list_dir",
      "List a directory; small workspace-only text directories include files.",
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

  Tool& grep = path_tool(
      MakeTool("grep", "Regex-search files; optional path/glob.",
               schema(R"json({"type":"object","properties":{
                    "pattern":{"type":"string"},"path":{"type":"string"},
                    "glob":{"type":"string"}},"required":["pattern"]})json"),
               [&supervisor](const json& a, const ToolContext& context) {
                 return ToolGrep(supervisor, JsonValue(a, "pattern", ""),
                                 JsonValue(a, "path", "."),
                                 JsonValue(a, "glob", ""), context);
               }));
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
                 }));
  }

  // read_file only handles text. This puts the bytes themselves in front of
  // the model, so it can read what it cannot parse.
  Tool& attach = path_tool(MakeTool(
      "attach",
      "Add an image/document to model context when read_file cannot parse it.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"}},"required":["path"]})json"),
      [](const json& a, const ToolContext&) {
        return g_attachments.Add(JsonValue(a, "path", ""));
      }));
  attach.parallel_safe = true;
  attach.max_calls_per_turn = 4;  // each one rides on the next request

  Tool& run = AddTool(
      tools,
      MakeTool("run",
               "Run a command to completion in cwd (bash default; omit cd; no "
               "stdin). Use detach only for persistent terminal_output.",
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
  run.summary = [](const json& a) { return JsonValue(a, "command", ""); };
  run.timeout_s = 0;  // bounded by the turn; Escape remains responsive
  // Each call owns its process group and log, so independent commands
  // (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;

  Tool& python = AddTool(
      tools,
      MakeTool(
          "run_python",
          "Run isolated uv Python; list third-party packages. Environments "
          "cache; pip/venv state does not. Save plots for show_image.",
          schema(R"json({"type":"object","properties":{
                    "code":{"type":"string"},
                    "packages":{"type":"array","items":{"type":"string"},"maxItems":12,
                      "description":"PEP 508 requirements; omit stdlib"}},
                    "required":["code"]})json"),
          [&supervisor](const json& a, const ToolContext& context) {
            return ToolRunPython(supervisor, JsonValue(a, "code", ""),
                                 JsonValue(a, "packages", json::array()),
                                 context);
          }));
  python.mutating = true;
  python.summary = [](const json& a) {
    std::string summary = JsonValue(a, "code", "");
    size_t packages = JsonValue(a, "packages", json::array()).size();
    return packages ? summary + "\n[packages: " + JsonDump(a["packages"]) + "]"
                    : summary;
  };
  python.timeout_s = 0;  // bounded by the turn; no model-driven polling

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
  terminal.visibility = Tool::Visibility::kDetachedTerminal;
  terminal.summary = [](const json& a) {
    int64_t pid = JsonValue(a, "pid", int64_t{0});
    return pid ? "pid " + std::to_string(pid) : "list";
  };

  Tool& memory = AddTool(
      tools,
      MakeTool(
          "memory",
          "Save, replace, or forget a durable project/global lesson. Use only "
          "facts that remain true across sessions.",
          schema(R"json({"type":"object","properties":{
                    "name":{"type":"string","description":"kebab-case slug; reuse replaces"},
                    "scope":{"type":"string","enum":["project","global"],
                      "description":"project workspace or all workspaces"},
                    "content":{"type":"string",
                      "description":"markdown; omit to forget"}},
                    "required":["name","scope"]})json"),
          [](const json& a, const ToolContext&) {
            return ToolMemory(JsonValue(a, "name", ""),
                              JsonValue(a, "scope", ""),
                              JsonValue(a, "content", ""));
          }));
  memory.mutating = true;
  memory.available_in_lean = false;
  memory.summary = [](const json& a) {
    return JsonValue(a, "scope", "") + "/" + JsonValue(a, "name", "");
  };

  Tool& checkpoint = AddTool(
      tools,
      MakeTool("checkpoint",
               "After a checkpoint hint, save durable facts and validation—not "
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
                 return ToolFailure(
                     ToolErrorCode::kInternal,
                     "error: checkpoint must be handled by the agent runtime");
               }));
  checkpoint.summary = [](const json& a) {
    return FirstLine(JsonValue(a, "state", ""));
  };
  checkpoint.visibility = Tool::Visibility::kCheckpointHint;
  return tools;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_REGISTRY_H_
