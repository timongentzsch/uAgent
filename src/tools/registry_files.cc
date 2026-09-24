// Copyright 2026 Timon Gentzsch

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
#include "include/tools/shell.h"
#include "src/tools/registry_internal.h"

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

void RegisterFileTools(std::vector<Tool>& tools, ProcessSupervisor& supervisor,
                       const std::filesystem::path& workspace) {
  auto schema = [](const char* s) { return json::parse(s); };
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
  // Scripts under .uagent/scratch are the agent's own working files: writing
  // one changes nothing a person relies on. Running it is what asks, and the
  // scratch approval shows the script.
  auto outside_scratch = [workspace](const json& args) {
    return !PathWithin(
        CanonicalAccessPath(JsonValue(args, "path", "")),
        CanonicalAccessPath((workspace / ".uagent" / "scratch").string()));
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
      "Read text/ranges, list directories, or add images/documents to model "
      "context. Omit ranges for media. Use grep for unknown "
      "paths or symbols. Reread only after changes when current text matters.",
      schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "offset":{"type":"integer","description":"first line or entry (default 1)"},
                    "limit":{"type":"integer","description":"lines or entries (default 1000)"}},
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
  // Reads get a larger, contiguous window than logs and remote output. This
  // avoids paying another model round merely to continue an ordinary source
  // file while keeping every other tool on the global result cap.
  read.result_chars = ReadFileResultChars();

  Tool& write = path_tool(MakeTool(
      "write_file",
      "Create a file. Use edit_file for existing files; overwrite=true "
      "explicitly permits whole-file replacement.",
      schema(R"json({"type":"object","properties":{
                  "path":{"type":"string"},
                  "content":{"type":"string"},
                  "overwrite":{"type":"boolean"}},
                  "required":["path","content"]})json"),
      [](const json& a, const ToolContext&) {
        return ToolWriteFileWithDisplay(JsonValue(a, "path", ""),
                                        JsonValue(a, "content", ""),
                                        JsonValue(a, "overwrite", false));
      }));
  write.mutates = outside_scratch;
  write.capabilities = Capability(ToolCapability::kMutate);
  write.available_in_lean = false;
  write.summary = [](const json& a) {
    return "write " + JsonValue(a, "path", "") + " · " +
           FmtBytes(static_cast<int64_t>(
               JsonValue(a, "content", std::string()).size()));
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
  edit.mutates = outside_scratch;
  edit.capabilities = Capability(ToolCapability::kMutate);
  edit.available_in_lean = false;
  edit.summary = [](const json& a) {
    size_t count = 0;
    auto edits = a.find("edits");
    if (edits != a.end() && edits->is_array()) count = edits->size();
    return "edit " + JsonValue(a, "path", "") + " · " + std::to_string(count) +
           (count == 1 ? " edit" : " edits");
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
      "Search regex or literal text. mode: content returns lines; files "
      "matches paths; matching_files returns paths whose contents match.",
      schema(R"json({"type":"object","properties":{
                    "pattern":{"type":"string","minLength":1},"path":{"type":"string"},
                    "glob":{"type":"string"},
                    "mode":{"type":"string","enum":["content","files","matching_files"]},
                    "literal":{"type":"boolean"},
                    "context":{"type":"integer","minimum":0,"maximum":10,
                      "description":"surrounding content lines"}},"required":["pattern"]})json"),
      [&supervisor](const json& a, const ToolContext& context) {
        return ToolGrep(supervisor, JsonValue(a, "pattern", ""),
                        JsonValue(a, "path", "."), JsonValue(a, "glob", ""),
                        JsonValue(a, "context", int64_t{0}), context,
                        JsonValue(a, "mode", "content") == "files",
                        JsonValue(a, "literal", false),
                        JsonValue(a, "mode", "content") == "matching_files");
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
  // Same contract as read_path: only a byte-identical repeat of a result still
  // in recent context collapses to a receipt, so a search that found anything
  // new is always resent in full.
  grep.dedupe_output = true;
  grep.summary = [](const json& a) {
    std::string mode = JsonValue(a, "mode", "content");
    return "search " + (mode == "files" ? std::string("files ") : "") + "/" +
           JsonValue(a, "pattern", "") + "/ in " + JsonValue(a, "path", ".");
  };
}

}  // namespace uagent
