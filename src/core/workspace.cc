// Copyright 2026 Timon Gentzsch
#include "include/core/workspace.h"

#include <utility>

#include "include/core/fs.h"
#include "include/core/strings.h"
namespace uagent {
namespace {
WorkspaceIdentity workspace;
std::string execution_state;
std::function<json(const json&)> workspace_request;
}  // namespace
std::string WorkspaceIdentity::Key() const {
  return host.empty() ? path : "ssh:" + host + ":" + path;
}
WorkspaceIdentity Workspace() {
  return workspace.path.empty() ? WorkspaceIdentity{"", CanonicalCwd()}
                                : workspace;
}
void SetWorkspaceRequest(std::function<json(const json&)> request) {
  workspace_request = std::move(request);
}
json WorkspaceRequest(const json& request) {
  return workspace_request
             ? workspace_request(request)
             : json{{"error", "Remote execution is unavailable."}};
}
bool ReadWorkspaceFile(const std::string& path, size_t limit,
                       std::string& bytes, std::string& error) {
  if (!Workspace().Remote()) return ReadRegularFile(path, limit, bytes, error);
  const json result = WorkspaceRequest({{"op", "bytes"}, {"path", path}});
  if (HexDecode(JsonValue(result, "data", ""), bytes, limit) &&
      result.contains("data"))
    return true;
  error = JsonValue(result, "error", "Cannot read remote file.");
  return false;
}
void SetWorkspace(WorkspaceIdentity value) { workspace = std::move(value); }
void SetExecutionStateDirectory(std::string value) {
  execution_state = std::move(value);
}
const std::string& ExecutionStateDirectory() { return execution_state; }
}  // namespace uagent
