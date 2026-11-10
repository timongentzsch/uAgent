// Copyright 2026 Timon Gentzsch

#include "include/app/artifact.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/app/asset_store.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"
#include "src/app/bootstrap_internal.h"

namespace uagent {

Tool ArtifactTool(const std::string& session_path) {
  Tool tool = MakeTool(
      "artifact",
      "Hand the user a file you made (HTML report, PDF, image, archive, "
      "data) to open or download from the conversation. Snapshots the file "
      "as it is now; share again after changing it.",
      json::parse(R"json({"type":"object","additionalProperties":false,
        "properties":{"path":{"type":"string","minLength":1}},
        "required":["path"]})json"),
      [session_path](const json& args, const ToolContext&) {
        session::AssetStoreResult stored =
            StoreSessionFile(session_path, JsonValue(args, "path", ""), "",
                             /*tool_copy=*/false, /*shared=*/true);
        // A file that cannot be read is the call's mistake; one the store
        // refuses (status set) is not.
        if (!stored.error.empty()) {
          return ToolFailure(stored.status == 200
                                 ? ToolErrorCode::kInvalidArguments
                                 : ToolErrorCode::kInternal,
                             stored.error);
        }
        ToolResult result =
            ToolSuccess("shared " + JsonValue(stored.value, "name", "") + " (" +
                        FmtCount(JsonValue(stored.value, "bytes", int64_t{0})) +
                        " bytes); it is shown in the conversation, where the "
                        "user can open or download it");
        stored.value["kind"] = "file";
        result.parts = json::array({std::move(stored.value)});
        return result;
      });
  tool.headline = [](const json& args) {
    return Headline{"Sharing", "Shared", JsonValue(args, "path", "")};
  };
  // Sharing reads the file out to the person's devices: the same path policy
  // as read_path, so a file outside the folder or µAgent's own
  // configuration never leaves without approval.
  tool.needs_approval = [](const json& args) {
    return PathApprovalRequired(JsonValue(args, "path", ""), CanonicalCwd());
  };
  tool.approval_class = [](const json& args) {
    return PathApprovalClass(JsonValue(args, "path", ""), PathAccess::kRead);
  };
  tool.validate = RefuseHidden;
  if (Workspace().Remote()) {
    auto check = [](const json& args) {
      return WorkspaceRequest(
          {{"op", "check"}, {"tool", "read_path"}, {"args", args}});
    };
    tool.needs_approval = [check](const json& args) {
      return JsonValue(check(args), "approval", 2) != 0;
    };
    tool.approval_class = [check](const json& args) {
      return JsonValue(check(args), "approval", 2) == 2
                 ? ApprovalClass::kMandatoryHuman
                 : ApprovalClass::kNone;
    };
    tool.validate =
        [check](const json& args) -> std::optional<ToolArgumentIssue> {
      const json value = check(args);
      if (value.contains("error"))
        return ArgumentIssue("remote.unavailable",
                             JsonValue(value, "error", "Remote unavailable"));
      if (const json* issue = JsonObject(value, "issue"))
        return ArgumentIssue(
            JsonValue(*issue, "code", "remote.invalid"),
            JsonValue(*issue, "message", "Invalid remote path"));
      return std::nullopt;
    };
  } else
    tool.run = RefusingHidden(std::move(tool.run));
  tool.capabilities = Capability(ToolCapability::kInspect);
  tool.parallel_safe = true;
  return tool;
}

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
    if (!args.contains("from")) {
      if (JsonStringRef(args, "content")) return run(args, context);
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "write_file needs content, or from to copy an "
                         "attached file");
    }
    json copy = args;
    std::string data;
    if (!bytes(args, data)) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "from names no file attached to this conversation");
    }
    copy[Workspace().Remote() ? "content_hex" : "content"] =
        Workspace().Remote() ? HexEncode(data) : std::move(data);
    copy.erase("from");
    return run(copy, context);
  };
  if (Workspace().Remote()) {
    auto prepare = [](json args) {
      if (args.contains("from")) {
        args.erase("from");
        args["content"] = "";
      }
      return args;
    };
    write->validate = [validate = std::move(write->validate), prepare](
                          const json& args) { return validate(prepare(args)); };
    write->canonicalize = [canonicalize = std::move(write->canonicalize),
                           prepare](json& args) {
      json checked = prepare(args);
      canonicalize(checked);
      if (args.contains("from")) checked["from"] = args["from"];
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
    return args.contains("from") ? "a copy of " + JsonValue(args, "from", "")
                                 : preview(args);
  };
}

}  // namespace uagent
