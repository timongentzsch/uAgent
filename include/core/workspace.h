// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_CORE_WORKSPACE_H_
#define UAGENT_INCLUDE_CORE_WORKSPACE_H_
#include <functional>
#include <string>

#include "include/core/json.h"

namespace uagent {
// Logical execution identity. A remote path must never be resolved on the main.
struct WorkspaceIdentity {
  std::string host, path;
  bool Remote() const { return !host.empty(); }
  std::string Key() const;
};
WorkspaceIdentity Workspace();
// Whether this process works on another host; no path is resolved to ask.
bool RemoteWorkspace();
void SetWorkspaceRequest(std::function<json(const json&)> request);
json WorkspaceRequest(const json& request);
bool ReadWorkspaceFile(const std::string& path, size_t limit,
                       std::string& bytes, std::string& error);
// Set once before a conversation worker starts any threads.
void SetWorkspace(WorkspaceIdentity workspace);
// Runtime-only storage for an execution worker; never the account's ~/.uagent.
void SetExecutionStateDirectory(std::string directory);
const std::string& ExecutionStateDirectory();
}  // namespace uagent
#endif
