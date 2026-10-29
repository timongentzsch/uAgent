// Copyright 2026 Timon Gentzsch

#include "include/app/artifact.h"

#include <string>
#include <utility>

#include "include/agent/path_policy.h"
#include "include/app/asset_store.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
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
                             /*tool_copy=*/false);
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
  tool.intent = "share";
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
  tool.run = RefusingHidden(std::move(tool.run));
  tool.capabilities = Capability(ToolCapability::kInspect);
  tool.parallel_safe = true;
  return tool;
}

}  // namespace uagent
