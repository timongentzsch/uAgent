// Copyright 2026 Timon Gentzsch

#include "include/tools/registry.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/media.h"
#include "include/tools/adapt_system.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/tools/path_policy.h"
#include "include/tools/shell.h"

namespace uagent {
namespace {

// The preview and the edit itself read the same arguments; parsing them twice
// is how the two drifted apart in the first place.
std::vector<FileEdit> RequestedEdits(const json& arguments) {
  std::vector<FileEdit> edits;
  for (const json& item : arguments["edits"]) {
    edits.push_back({JsonValue(item, "old", ""), JsonValue(item, "new", ""),
                     JsonValue(item, "replace_all", false)});
  }
  return edits;
}

}  // namespace

std::vector<Tool> BuiltinTools(ProcessSupervisor& supervisor,
                               const std::filesystem::path& workspace,
                               bool inline_images,
                               AdaptiveSystemState* adaptive_system) {
  auto schema = [](const char* s) { return json::parse(s); };
  std::vector<Tool> tools;
  if (adaptive_system) tools.push_back(AdaptSystemTool(*adaptive_system));
  // "." is what a path-less read_path or grep operates on, so the hooks judge
  // the path the call actually reaches. Access is assumed to be the stricter
  // half, so a tool added later escalates until someone marks it read-only.
  auto path_tool = [&](Tool tool) -> Tool& {
    tool.needs_approval = [workspace](const json& args) {
      return PathApprovalRequired(JsonValue(args, "path", "."), workspace);
    };
    tool.approval_class = [](const json& args) {
      return PathApprovalClass(JsonValue(args, "path", "."),
                               PathAccess::kWrite);
    };
    return AddTool(tools, std::move(tool));
  };
  auto reads_only = [](Tool& tool) {
    tool.approval_class = [](const json& args) {
      return PathApprovalClass(JsonValue(args, "path", "."), PathAccess::kRead);
    };
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
        int64_t offset = JsonValue(a, "offset", int64_t{1});
        int64_t limit = JsonValue(a, "limit", int64_t{0});
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
          return ToolListDir(path, offset, limit,
                             !PathApprovalRequired(path, workspace));
        }
        return ToolReadFile(path, offset > 0 ? offset : 1, limit);
      }));
  reads_only(read);
  read.parallel_safe = true;
  read.capabilities = Capability(ToolCapability::kInspect);
  read.dedupe_output = true;
  // Reads get a larger, contiguous window than logs and remote output. This
  // avoids paying another model round merely to continue an ordinary source
  // file while keeping every other tool on the global result cap.
  read.result_chars = ReadFileResultChars();

  Tool& write = path_tool(MakeTool(
      "write_file",
      "Create a file or replace it whole. Use edit_file for changes to an "
      "existing file.",
      schema(R"json({"type":"object","properties":{
                  "path":{"type":"string"},
                  "content":{"type":"string"}},
                  "required":["path","content"]})json"),
      [](const json& a, const ToolContext&) {
        return ToolWriteFileWithDisplay(JsonValue(a, "path", ""),
                                        JsonValue(a, "content", ""));
      }));
  write.mutating = true;
  write.capabilities = Capability(ToolCapability::kMutate);
  write.available_in_lean = false;
  write.summary = [](const json& a) {
    return JsonValue(a, "path", "") + " (" +
           FmtBytes(static_cast<int64_t>(
               JsonValue(a, "content", std::string()).size())) +
           ")";
  };
  write.approval_preview = [](const json& a) {
    std::string path = JsonValue(a, "path", "");
    std::string content = JsonValue(a, "content", "");
    std::error_code ec;
    bool existed = std::filesystem::is_regular_file(path, ec);
    std::optional<std::string> prev = DiffableContents(path);
    if (!prev) prev.emplace();
    std::string diff = WholeFileDiffDisplay(path, *prev, content, existed);
    return diff.empty() ? "no changes" : diff;
  };

  Tool& edit = path_tool(MakeTool(
      "edit_file",
      "Apply exact search/replacements to an existing file, batched and "
      "atomic in order.",
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
        return ToolEditFile(JsonValue(a, "path", ""), RequestedEdits(a));
      }));
  edit.mutating = true;
  edit.capabilities = Capability(ToolCapability::kMutate);
  edit.available_in_lean = false;
  edit.summary = [](const json& a) {
    size_t count = 0;
    auto edits = a.find("edits");
    if (edits != a.end() && edits->is_array()) count = edits->size();
    return JsonValue(a, "path", "") + " (" + std::to_string(count) +
           (count == 1 ? " edit)" : " edits)");
  };
  edit.approval_preview = [](const json& a) {
    std::string path = JsonValue(a, "path", "");
    auto prev = DiffableContents(path);
    if (!prev) return "edit " + DisplayPath(path);
    std::string data = *prev;
    // Refusing is part of what the human is approving: show the error the
    // edit would return rather than a diff of the edits that precede it.
    if (auto refusal = ApplyFileEdits(data, path, RequestedEdits(a))) {
      return refusal->output;
    }
    std::string diff = WholeFileDiffDisplay(path, *prev, data, true);
    return diff.empty() ? "no effective changes" : diff;
  };
  Tool& remove = path_tool(MakeTool(
      "delete_file",
      "Delete a regular file and show its removed content as a red diff.",
      schema(R"json({"type":"object","properties":{
                  "path":{"type":"string"}},"required":["path"]})json"),
      [](const json& a, const ToolContext&) {
        return ToolDeleteFileWithDisplay(JsonValue(a, "path", ""));
      }));
  remove.mutating = true;
  remove.capabilities = Capability(ToolCapability::kMutate);
  remove.available_in_lean = false;
  remove.approval_preview = [](const json& a) {
    std::string path = JsonValue(a, "path", "");
    auto prev = DiffableContents(path);
    if (!prev || prev->empty()) return "delete " + DisplayPath(path);
    return DeletedFileDiffDisplay(path, *prev);
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
  reads_only(grep);
  grep.clamped_arguments = {"context"};
  grep.canonicalize = [](json& arguments) {
    if (JsonValue(arguments, "mode", "content") == "files") {
      arguments.erase("context");
    }
  };
  grep.parallel_safe = true;  // read-only, like read_path
  grep.capabilities = Capability(ToolCapability::kInspect);
  // Same contract as read_path: only a byte-identical repeat of a result still
  // in recent context collapses to a receipt, so a search that found anything
  // new is always resent in full.
  grep.dedupe_output = true;
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
    reads_only(show_image);
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
        return Attachments().Add(
            JsonValue(a, "path", ""), context.image_input_available,
            context.image_fallback_available, context.call_id);
      }));
  reads_only(attach);
  attach.parallel_safe = true;
  attach.capabilities = Capability(ToolCapability::kInspect);
  // No per-turn cap of its own: the queue ceiling and the byte budget already
  // bound what one request can carry, and they reject with the reason rather
  // than hiding the tool once a count is reached.

  // The schema below is a raw JSON literal, so its "maximum" cannot be spelled
  // as kMaxYieldMs directly; this assert fails the build if the constant moves.
  static_assert(kMaxYieldMs == 30000, "update \"maximum\" in the run schema");
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
  run.timeout_s = 0;  // bounded by the turn; Escape remains responsive
  // Each call owns its process group and log, so independent commands
  // (network fetches especially) overlap instead of queueing.
  run.parallel_safe = true;
  run.command_policy = true;

  // ToolRunScratch runs a .py under uv when it is there and falls back to
  // python3 otherwise, so a host with neither can only ever answer this tool
  // with an error. An 800-byte schema that cannot succeed is worse than an
  // absent one, and the same reasoning already gates show_image. A .sh needs
  // only sh, but gating the whole tool on the interpreter its Python half
  // needs keeps one condition instead of two.
  if (ExecutableOnPath("uv") || ExecutableOnPath("python3")) {
    Tool& python = AddTool(
        tools,
        MakeTool(
            "scratch",
            "Run a one-off script, never requested project code. Writes or "
            "replaces one persistent script under .uagent/scratch and reruns "
            "it by path with optional argv: prefer this over resending a long "
            "shell pipeline or heredoc through run, and vary `args` instead "
            "of rewriting the body. .py runs under isolated uv, .sh under sh.",
            schema(
                R"json({"type":"object","additionalProperties":false,"properties":{
                    "path":{"type":"string","minLength":1,
                      "description":"stable relative .py or .sh path; reuse it during the task"},
                    "code":{"type":["string","null"],"minLength":1,"maxLength":131072,
                      "description":"script body, without PEP 723 metadata; null reruns the file unchanged"},
                    "packages":{"type":["array","null"],"items":{"type":"string","minLength":1,"maxLength":256},"maxItems":12,
                      "description":"PEP 508 dependencies with code ([] for stdlib or any .sh); null when rerunning"},
                    "args":{"type":"array","items":{"type":"string","maxLength":4096},"maxItems":32,
                      "description":"argv for this run, read from sys.argv or $@; vary it instead of rewriting the script"}},
                    "required":["path","code","packages"]})json"),
            [&supervisor, workspace](const json& a,
                                     const ToolContext& context) {
              return ToolRunScratch(
                  supervisor, workspace, JsonValue(a, "path", ""),
                  JsonValue(a, "code", json(nullptr)),
                  JsonValue(a, "packages", json(nullptr)),
                  JsonValue(a, "args", json(nullptr)), context);
            }));
    python.mutating = true;
    python.capabilities = Capability(ToolCapability::kExecute) |
                          Capability(ToolCapability::kMutate);
    python.summary = [](const json& a) {
      std::string path = JsonValue(a, "path", "");
      std::string argv = ScratchArgvLabel(JsonValue(a, "args", json(nullptr)));
      return a.contains("code") && a["code"].is_string()
                 ? "write/replace " + path + " → execute" + argv
                 : "execute " + path + argv;
    };
    python.stable_argument = "path";
    python.timeout_s = 0;  // bounded by the turn; no model-driven polling
  }

  Tool& activity = AddTool(
      tools,
      MakeTool(
          "activity",
          "Inspect or drive activities with an explicit operation: list, poll "
          "one, wait for any/all, write to one, resize its PTY, or stop one "
          "— stop terminates its complete process group and cleans its log. "
          "Completion never starts a model turn.",
          schema(
              R"json({"type":"object","additionalProperties":false,"properties":{
                  "operation":{"type":"string","enum":["list","poll","wait","write","resize","stop"]},
                  "id":{"type":"integer","minimum":1,"maximum":2147483647,
                    "description":"activity for poll, write, resize, or stop"},
                  "chars":{"type":"string","maxLength":65536,
                    "description":"bytes for write; empty is intentional"},
                  "wait_ms":{"type":"integer","minimum":0,"maximum":300000},
                  "until":{"type":"string","maxLength":256,
                    "description":"readiness marker for poll"},
                  "mode":{"type":"string","enum":["any","all"],
                    "description":"completion mode for wait"},
                  "rows":{"type":"integer","description":"PTY rows in 1..1000"},
                  "cols":{"type":"integer","description":"PTY columns in 1..1000"},
                  "max_output_chars":{"type":"integer","minimum":256,"maximum":65536}},
                  "required":["operation"]})json"),
          [&supervisor](const json& a, const ToolContext& context) {
            std::string operation = JsonValue(a, "operation", "");
            int64_t id = JsonValue(a, "id", int64_t{0});
            int64_t wait_ms = JsonValue(a, "wait_ms", int64_t{0});
            int64_t cap = JsonValue(a, "max_output_chars", int64_t{0});
            if (operation == "list") {
              return ToolActivityOutput(supervisor, 0, 0, {}, context, cap);
            }
            if (operation == "poll") {
              return ToolActivityOutput(supervisor, id, wait_ms,
                                        JsonValue(a, "until", ""), context,
                                        cap);
            }
            if (operation == "wait") {
              return ToolActivityWait(supervisor, {},
                                      JsonValue(a, "mode", "any"), wait_ms,
                                      context, cap);
            }
            if (operation == "write") {
              return ToolActivityInput(
                  supervisor, id, JsonValue(a, "chars", ""),
                  a.contains("wait_ms") ? wait_ms : kActivityInputSettleMs,
                  context, 0, 0, cap);
            }
            if (operation == "resize") {
              return ToolActivityInput(
                  supervisor, id, "",
                  a.contains("wait_ms") ? wait_ms : kActivityInputSettleMs,
                  context, JsonValue(a, "rows", int64_t{0}),
                  JsonValue(a, "cols", int64_t{0}), cap);
            }
            if (operation == "stop") return ToolActivityStop(supervisor, id);
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: unknown activity operation");
          }));
  activity.canonicalize = [](json& a) {
    std::string operation = JsonValue(a, "operation", "");
    if (operation != "list" && operation != "poll" && operation != "wait" &&
        operation != "write" && operation != "resize" && operation != "stop") {
      return;
    }
    auto relevant = [&](std::string_view field) {
      if (field == "id") {
        return operation == "poll" || operation == "write" ||
               operation == "resize" || operation == "stop";
      }
      if (field == "chars") return operation == "write";
      if (field == "wait_ms") return operation != "list" && operation != "stop";
      if (field == "until") return operation == "poll";
      if (field == "mode") return operation == "wait";
      if (field == "rows" || field == "cols") return operation == "resize";
      return false;
    };
    for (std::string_view field :
         {"id", "chars", "wait_ms", "until", "mode", "rows", "cols"}) {
      if (!relevant(field)) a.erase(std::string(field));
    }
    auto cap = a.find("max_output_chars");
    if (cap != a.end() && cap->is_number_integer() &&
        cap->get<int64_t>() <= 0) {
      a.erase(cap);
    }
  };
  activity.clamped_arguments = {"wait_ms", "max_output_chars"};
  // Waiting is not work: no process runs, nothing is held. The tool timeout
  // exists to stop a command from running away, and applying it here truncated
  // the caller's own wait_ms — the schema offers five minutes and the default
  // budget granted thirty seconds, so a long job cost a round every half
  // minute. `run` and `scratch`, which do occupy a process, are already exempt
  // for the same reason. The turn deadline still bounds this, and Escape and
  // queued steering still return immediately.
  activity.timeout_s = 0;
  activity.parallel_safe = true;
  activity.capabilities = Capability(ToolCapability::kInspect) |
                          Capability(ToolCapability::kExecute) |
                          Capability(ToolCapability::kMutate);
  activity.mutates = [](const json& a) {
    std::string operation = JsonValue(a, "operation", "");
    return operation == "write" || operation == "resize" || operation == "stop";
  };
  activity.result_chars = kActivityResultChars;
  activity.blocking_wait_default_ms = 0;
  activity.visibility = Tool::Visibility::kDetachedTerminal;
  activity.validate = [](const json& a) -> std::optional<ToolArgumentIssue> {
    std::string operation = JsonValue(a, "operation", "");
    if ((operation == "poll" || operation == "write" || operation == "resize" ||
         operation == "stop") &&
        !a.contains("id")) {
      return ArgumentIssue("activity.missing_id", operation + " requires id",
                           "id");
    }
    if (operation == "write" && !a.contains("chars")) {
      return ArgumentIssue("activity.missing_chars", "write requires chars",
                           "chars");
    }
    if (operation == "wait" && !a.contains("wait_ms")) {
      return ArgumentIssue("activity.missing_wait", "wait requires wait_ms",
                           "wait_ms");
    }
    if (operation == "resize") {
      int64_t rows = JsonValue(a, "rows", int64_t{0});
      int64_t cols = JsonValue(a, "cols", int64_t{0});
      if (rows < 1 || rows > 1000 || cols < 1 || cols > 1000) {
        return ArgumentIssue("activity.invalid_dimensions",
                             "resize requires rows and cols in 1..1000");
      }
    }
    return std::nullopt;
  };
  activity.summary = [](const json& a) {
    std::string operation = JsonValue(a, "operation", "");
    int64_t id = JsonValue(a, "id", int64_t{0});
    if (operation.empty()) {
      if (a.contains("chars")) {
        operation = "write";
      } else if (a.contains("rows") || a.contains("cols")) {
        operation = "resize";
      } else if (id > 0) {
        operation = "poll";
      } else if (JsonValue(a, "wait_ms", int64_t{0}) > 0) {
        operation = "wait";
      } else {
        operation = "list";
      }
    }
    int64_t wait_ms = JsonValue(a, "wait_ms", int64_t{0});
    std::string wait =
        wait_ms > 0
            ? " · wait≤" + FmtDuration(static_cast<double>(wait_ms) / 1000.0)
            : std::string();
    if (operation == "list") return std::string("list activities");
    if (operation == "wait") {
      return "wait for " + JsonValue(a, "mode", "any") + " · all current" +
             wait;
    }
    std::string target = "activity " + std::to_string(id);
    if (operation == "write") {
      return "write " +
             FmtBytes(static_cast<int64_t>(JsonValue(a, "chars", "").size())) +
             " → " + target + wait;
    }
    if (operation == "resize") {
      return "resize " + std::to_string(JsonValue(a, "rows", int64_t{0})) +
             "×" + std::to_string(JsonValue(a, "cols", int64_t{0})) + " → " +
             target + wait;
    }
    if (operation == "stop") return "stop " + target;
    if (operation == "poll" && a.contains("until")) {
      return "await " + TerminalSafe(JsonValue(a, "until", "")) + " · " +
             target + wait;
    }
    if (operation == "poll") return "poll " + target + wait;
    return std::string("activity");
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
