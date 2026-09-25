// Copyright 2026 Timon Gentzsch

#include "include/app/artifact.h"

#include <filesystem>
#include <string>
#include <utility>

#include "include/agent/path_policy.h"
#include "include/app/asset_store.h"
#include "include/app/session.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

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
        const std::string path = JsonValue(args, "path", "");
        std::string bytes, error;
        if (!ReadRegularFile(path, session::kUploadBytes, bytes, error)) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: " + error);
        }
        session::AssetStore store;
        session::AssetStoreResult stored = store.Store(
            session_path, bytes, std::filesystem::path(path).filename(),
            /*committed=*/true);
        if (!stored.error.empty()) {
          return ToolFailure(ToolErrorCode::kInternal,
                             "error: " + stored.error);
        }
        ToolResult result =
            ToolSuccess("shared " + JsonValue(stored.value, "name", "") + " (" +
                        FmtCount(static_cast<int64_t>(bytes.size())) +
                        " bytes); the user can open or download it");
        stored.value["kind"] = "file";
        result.parts = json::array({std::move(stored.value)});
        return result;
      });
  tool.summary = [](const json& args) { return JsonValue(args, "path", ""); };
  tool.header = Verbs("Sharing", "Shared");
  // Sharing reads the file out to the person's devices: the same path policy
  // as read_path, so a file outside the workspace or µAgent's own
  // configuration never leaves without approval.
  tool.needs_approval = [](const json& args) {
    return PathApprovalRequired(JsonValue(args, "path", ""), CanonicalCwd());
  };
  tool.approval_class = [](const json& args) {
    return PathApprovalClass(JsonValue(args, "path", ""), PathAccess::kRead);
  };
  tool.capabilities = Capability(ToolCapability::kInspect);
  tool.parallel_safe = true;
  return tool;
}

}  // namespace uagent
