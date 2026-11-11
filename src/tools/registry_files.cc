// Copyright 2026 Timon Gentzsch

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
#include "include/tools/shell.h"
#include "src/tools/registry_internal.h"

namespace uagent {
namespace {

// One parse for the preview and the edit itself, so the two cannot differ.
std::vector<FileEdit> RequestedEdits(const json& arguments) {
  std::vector<FileEdit> edits;
  for (const json& item : arguments["edits"]) {
    edits.push_back({JsonValue(item, "old", ""), JsonValue(item, "new", ""),
                     JsonValue(item, "replace_all", false)});
  }
  return edits;
}

}  // namespace

void RegisterFileTools(std::vector<Tool>& tools, ActivitySupervisor& supervisor,
                       const std::filesystem::path& workspace) {
  // "." is what a path-less read_path or grep operates on. Access defaults to
  // write, so a tool added later escalates until it is marked read-only.
  auto path_tool = [&](Tool tool) -> Tool& {
    tool.needs_approval = [workspace](const json& args) {
      return PathApprovalRequired(JsonValue(args, "path", "."), workspace);
    };
    tool.approval_class = [](const json& args) {
      return PathApprovalClass(JsonValue(args, "path", "."),
                               PathAccess::kWrite);
    };
    tool.validate = RefuseHidden;
    tool.run = RefusingHidden(std::move(tool.run));
    return AddTool(tools, std::move(tool));
  };
  // Scripts under .uagent/scratch are the agent's own working files: writing
  // one needs no approval; running one asks, and that approval shows it.
  auto outside_scratch = [workspace](const json& args) {
    return !PathWithin(CanonicalAccessPath(JsonValue(args, "path", "")),
                       CanonicalAccessPath(ScratchDir(workspace).string()));
  };
  // What uAgent wrote for the model to read, such as a spilled result, is
  // read without asking: a command reads it freely anyway.
  auto reads_only = [workspace](Tool& tool) {
    tool.needs_approval = [workspace](const json& args) {
      const std::string path = JsonValue(args, "path", ".");
      return PathApprovalRequired(path, workspace) &&
             !PathWithin(CanonicalAccessPath(path),
                         CanonicalAccessPath(UagentDir(kArtifactsDir)));
    };
    tool.approval_class = [](const json& args) {
      return PathApprovalClass(JsonValue(args, "path", "."), PathAccess::kRead);
    };
  };

  Tool& read = path_tool(MakeTool(
      "read_path",
      "Read text/ranges, list directories, or add images/documents to model "
      "context. Omit ranges for media. Use grep for unknown "
      "paths or symbols. Reread only after changes when current text matters.",
      json::parse(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "offset":{"type":"integer","description":"first line or entry (default 1)"},
                    "limit":{"type":"integer","description":"lines or entries"}},
                    "required":["path"]})json"),
      [workspace](const json& a, const ToolContext& context) {
        std::string path = JsonValue(a, "path", ".");
        int64_t offset = JsonValue(a, "offset", int64_t{1});
        int64_t limit = JsonValue(a, "limit", int64_t{0});
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
          return ToolListDir(path, offset, limit,
                             !PathApprovalRequired(path, workspace));
        }
        return ToolReadFile(path, offset > 0 ? offset : 1, limit,
                            context.call_id);
      }));
  reads_only(read);
  read.parallel_safe = true;
  read.capabilities = Capability(ToolCapability::kInspect);
  read.dedupe_output = true;
  // Reads get a larger window than other tools, so an ordinary source file
  // does not cost another model round to continue.
  read.result_chars = ReadFileResultChars();
  read.headline = Verbs("Reading", "Read");

  Tool& write = path_tool(MakeTool(
      "write_file",
      "Create a file. Use edit_file for existing files; overwrite=true "
      "explicitly permits whole-file replacement.",
      json::parse(R"json({"type":"object","properties":{
                  "path":{"type":"string"},
                  "content":{"type":"string"},
                  "overwrite":{"type":"boolean"}},
                  "required":["path","content"]})json"),
      [workspace, outside_scratch](const json& a, const ToolContext&) {
        static const std::string kNone;
        const std::string* content = JsonStringRef(a, "content");
        // Its own scripts are rewritten as freely as they are written.
        ToolResult result = ToolWriteFileWithDisplay(
            JsonValue(a, "path", ""), content ? *content : kNone,
            JsonValue(a, "overwrite", false) || !outside_scratch(a));
        const std::filesystem::path ignore =
            ScratchDir(workspace) / ".gitignore";
        std::error_code ec;
        std::string error;
        if (result.Ok() && !outside_scratch(a) &&
            !std::filesystem::exists(ignore, ec)) {
          AtomicWriteFile(ignore.string(), "*\n", kSharedFileMode,
                          /*preserve_mode=*/false, error);
        }
        return result;
      }));
  write.mutates = outside_scratch;
  write.capabilities = Capability(ToolCapability::kMutate);
  write.headline = [](const json& a) {
    return Headline{"Writing", "Wrote", JsonValue(a, "path", "")};
  };
  write.approval_preview = [](const json& a) {
    static const std::string kNone;
    const std::string* content = JsonStringRef(a, "content");
    return WriteDiffPreview(JsonValue(a, "path", ""),
                            content ? *content : kNone);
  };

  Tool& edit = path_tool(MakeTool(
      "edit_file",
      "Apply exact search/replacements to an existing file, batched and "
      "atomic in order.",
      json::parse(R"json({"type":"object","properties":{
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
  edit.mutates = outside_scratch;
  edit.capabilities = Capability(ToolCapability::kMutate);
  edit.headline = [](const json& a) {
    return Headline{"Editing", "Edited", JsonValue(a, "path", "")};
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
      json::parse(R"json({"type":"object","properties":{
                  "path":{"type":"string"}},"required":["path"]})json"),
      [](const json& a, const ToolContext&) {
        return ToolDeleteFileWithDisplay(JsonValue(a, "path", ""));
      }));
  remove.mutating = true;
  remove.capabilities = Capability(ToolCapability::kMutate);
  remove.headline = [](const json& a) {
    return Headline{"Deleting", "Deleted", JsonValue(a, "path", "")};
  };
  remove.approval_preview = [](const json& a) {
    std::string path = JsonValue(a, "path", "");
    auto prev = DiffableContents(path);
    if (!prev || prev->empty()) return "delete " + DisplayPath(path);
    return DeletedFileDiffDisplay(path, *prev);
  };
  Tool& grep = path_tool(MakeTool(
      "grep",
      "Search file contents by regex (literal=true for fixed text). mode: "
      "content (default) returns matching lines; matching_files returns the "
      "files that contain a match; files matches the pattern against file "
      "paths instead of contents.",
      json::parse(R"json({"type":"object","properties":{
                    "pattern":{"type":"string","minLength":1},"path":{"type":"string"},
                    "glob":{"type":"string"},
                    "mode":{"type":"string","enum":["content","files","matching_files"]},
                    "literal":{"type":"boolean"},
                    "context":{"type":"integer","minimum":0,"maximum":10,
                      "description":"surrounding content lines"}},"required":["pattern"]})json"),
      [&supervisor](const json& a, const ToolContext& context) {
        return ToolGrep(
            supervisor, JsonValue(a, "pattern", ""), JsonValue(a, "path", "."),
            {.glob = JsonValue(a, "glob", ""),
             .context_lines = JsonValue(a, "context", int64_t{0}),
             .files_only = JsonValue(a, "mode", "content") == "files",
             .literal = JsonValue(a, "literal", false),
             .matching_files =
                 JsonValue(a, "mode", "content") == "matching_files"},
            context);
      }));
  reads_only(grep);
  grep.clamped_arguments = {"context"};
  grep.canonicalize = [](json& arguments) {
    if (JsonValue(arguments, "mode", "content") != "content") {
      arguments.erase("context");
    }
  };
  grep.parallel_safe = true;  // read-only, like read_path
  grep.capabilities = Capability(ToolCapability::kInspect);
  // As for read_path: only a byte-identical repeat of a result still in
  // recent context collapses to a receipt.
  grep.dedupe_output = true;
  grep.headline = [](const json& a) {
    const bool files = JsonValue(a, "mode", "content") == "files";
    return Headline{files ? "Searching file names for" : "Searching",
                    files ? "Searched file names for" : "Searched",
                    "/" + JsonValue(a, "pattern", "") + "/ in " +
                        JsonValue(a, "path", ".")};
  };
}

}  // namespace uagent
