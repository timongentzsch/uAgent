// Copyright 2026 Timon Gentzsch

#include "include/app/present.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/app/asset_store.h"
#include "include/app/workspace_files.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"
#include "src/app/bootstrap_internal.h"

namespace uagent {

namespace {
namespace fs = std::filesystem;

// The artifact at `path` as workspace_files' listing names it: a deck,
// design or design system for a folder or one of its pages, a document or a
// sheet for its own page. Null for another file, an error for a folder that
// is none.
json ArtifactPart(const std::string& path) {
  const auto list = [](const fs::path& dir) {
    const json request = {
        {"op", "files"}, {"mode", "list"}, {"path", dir.string()}};
    return RemoteWorkspace()
               ? WorkspaceRequest(request)
               : workspace_files::Request(CanonicalCwd(), request);
  };
  // The listing takes a path inside the folder; one outside is a file.
  fs::path target = fs::path(path).lexically_normal();
  if (target.is_absolute() && !RemoteWorkspace()) {
    target =
        fs::path(CanonicalAccessPath(path)).lexically_relative(CanonicalCwd());
  }
  if (!target.has_filename()) target = target.parent_path();
  json listing = list(target);
  const bool folder = listing.contains("entries");
  if (!folder) listing = list(target.parent_path());
  const std::string page = folder ? "" : target.filename().string();
  const fs::path dir = folder ? target : target.parent_path();
  std::string type = JsonValue(listing, "kind", ""), of;
  std::vector<std::string> pages;
  int64_t tabs = 0;
  for (const json& entry : JsonValue(listing, "entries", json::array())) {
    const std::string name = JsonValue(entry, "name", ""),
                      kind = JsonValue(entry, "kind", "");
    if (name == page) {
      of = kind;
      tabs = JsonValue(entry, "count", int64_t{0});
    }
    // A sheet is its own, and so is a document beside a deck or a design; a
    // system's document is its book.
    if (name.ends_with(".html") && kind != "sheet" &&
        (kind != "doc" || type == "system")) {
      pages.push_back(name);
    }
  }
  if (of == "sheet" || (of == "doc" && type != "system")) type = of;
  if (folder && (type.empty() || pages.empty())) {
    return {{"error", "nothing to present in " + path +
                          ": name a file, or a folder whose pages are a deck, "
                          "a design or a design system"}};
  }
  if (type.empty() || (!folder && of.empty())) return nullptr;
  const bool book =
      type == "system" && std::ranges::find(pages, "book.html") != pages.end();
  return {{"kind", "artifact"},
          {"type", type},
          {"path", folder ? target.string() + "/" : target.string()},
          {"cover",
           (dir / (folder ? book ? "book.html" : pages[0] : page)).string()},
          {"count", type == "sheet" ? tabs
                    : type == "doc" ? 1
                                    : static_cast<int64_t>(pages.size())}};
}

}  // namespace

Tool PresentTool(const std::string& session_path, const Tool* remote_read) {
  Tool tool = MakeTool(
      "present",
      "Hand the user what you made, shown in the conversation: an "
      "artifact's folder or page (document, deck, design, design system, "
      "sheet) "
      "opens in Files and stays live; any other file (PDF, image, archive, "
      "data) is snapshotted to open or download, so present it again after "
      "changing it.",
      json::parse(R"json({"type":"object","additionalProperties":false,
        "properties":{"path":{"type":"string","minLength":1}},
        "required":["path"]})json"),
      [session_path](const json& args, const ToolContext&) {
        const std::string path = JsonValue(args, "path", "");
        if (json part = ArtifactPart(path); part.contains("error")) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             JsonValue(part, "error", ""));
        } else if (!part.is_null()) {
          ToolResult result = ToolSuccess(
              "presented the " + JsonValue(part, "type", "") + " " +
              JsonValue(part, "path", "") +
              "; it is shown in the conversation, where the user can open it");
          result.parts = json::array({std::move(part)});
          return result;
        }
        session::AssetStoreResult stored =
            StoreSessionFile(session_path, path, "", /*tool_copy=*/false,
                             /*shared=*/true);
        // A file that cannot be read is the call's mistake; one the store
        // refuses (status set) is not.
        if (!stored.error.empty()) {
          return ToolFailure(stored.status == 200
                                 ? ToolErrorCode::kInvalidArguments
                                 : ToolErrorCode::kInternal,
                             stored.error);
        }
        ToolResult result = ToolSuccess(
            "presented " + JsonValue(stored.value, "name", "") + " (" +
            FmtCount(JsonValue(stored.value, "bytes", int64_t{0})) +
            " bytes); it is shown in the conversation, where the "
            "user can open or download it");
        stored.value["kind"] = "file";
        result.parts = json::array({std::move(stored.value)});
        return result;
      });
  tool.headline = [](const json& args) {
    return Headline{"Presenting", "Presented", JsonValue(args, "path", "")};
  };
  // The same path policy as read_path: a file outside the folder or µAgent's
  // own configuration never leaves without approval.
  tool.needs_approval = [](const json& args) {
    return PathApprovalRequired(JsonValue(args, "path", ""), CanonicalCwd());
  };
  tool.approval_class = [](const json& args) {
    return PathApprovalClass(JsonValue(args, "path", ""), PathAccess::kRead);
  };
  tool.validate = RefuseHidden;
  if (remote_read) {
    // The gateway judges the path where the file is (execution_client.cc).
    tool.needs_approval = remote_read->needs_approval;
    tool.approval_class = remote_read->approval_class;
    tool.approval_subject = remote_read->approval_subject;
    tool.validate = remote_read->validate;
  } else {
    tool.run = RefusingHidden(std::move(tool.run));
  }
  tool.capabilities = Capability(ToolCapability::kInspect);
  tool.parallel_safe = true;
  return tool;
}

namespace {
// An empty `from` is none: a model may fill every field of the schema.
bool CopiesAttachment(const json& args) {
  return !JsonValue(args, "from", "").empty();
}
}  // namespace

void CopyAttachments(std::vector<Tool>& tools,
                     const std::string& session_path) {
  auto write = std::find_if(tools.begin(), tools.end(), [](const Tool& tool) {
    return tool.name == "write_file";
  });
  if (write == tools.end() || session_path.empty()) return;
  write->parameters["properties"]["from"] = {
      {"type", "string"},
      {"description",
       "an attached file's path, as [Attached: …] names it, copied byte for "
       "byte in place of content"}};
  write->parameters["required"] = json::array({"path"});
  // The attachment's bytes, read from this conversation's own store only.
  const auto bytes = [session_path](const json& args, std::string& out) {
    const std::filesystem::path from =
        CanonicalAccessPath(JsonValue(args, "from", ""));
    const std::filesystem::path store =
        CanonicalAccessPath(session_path + kSessionAssetsSuffix);
    std::string error;
    return PathWithin(from, store) &&
           ReadRegularFile(from.string(), session::kUploadBytes, out, error);
  };
  write->run = [run = std::move(write->run), bytes](
                   const json& args, const ToolContext& context) {
    json copy = args;
    copy.erase("from");
    if (!CopiesAttachment(args)) {
      if (JsonStringRef(args, "content")) return run(copy, context);
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "write_file needs content, or from to copy an "
                         "attached file");
    }
    std::string data;
    if (!bytes(args, data)) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "from names no file attached to this conversation");
    }
    copy[RemoteWorkspace() ? "content_hex" : "content"] =
        RemoteWorkspace() ? HexEncode(data) : std::move(data);
    return run(copy, context);
  };
  if (RemoteWorkspace()) {
    auto prepare = [](json args) {
      if (CopiesAttachment(args)) args["content"] = "";
      args.erase("from");
      return args;
    };
    write->validate = [validate = std::move(write->validate), prepare](
                          const json& args) { return validate(prepare(args)); };
    write->canonicalize = [canonicalize = std::move(write->canonicalize),
                           prepare](json& args) {
      json checked = prepare(args);
      canonicalize(checked);
      if (CopiesAttachment(args)) checked["from"] = args["from"];
      args = std::move(checked);
    };
    write->mutates = [mutates = std::move(write->mutates), prepare](
                         const json& args) { return mutates(prepare(args)); };
    write->needs_approval = [needs = std::move(write->needs_approval),
                             prepare](const json& args) {
      return needs(prepare(args));
    };
    write->approval_class = [classify = std::move(write->approval_class),
                             prepare](const json& args) {
      return classify(prepare(args));
    };
    write->approval_subject = [subject = std::move(write->approval_subject),
                               prepare](const json& args) {
      return subject(prepare(args));
    };
  }
  write->approval_preview = [preview = std::move(write->approval_preview)](
                                const json& args) {
    return CopiesAttachment(args) ? "a copy of " + JsonValue(args, "from", "")
                                  : preview(args);
  };
}

}  // namespace uagent
