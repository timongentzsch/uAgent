// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_PRESENT_H_
#define UAGENT_INCLUDE_APP_PRESENT_H_
// The present tool: hands the person what the agent made.

#include <string>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {
// present(path): an artifact's folder or page is shown live from the folder;
// any other file is snapshotted into the session's asset store. `remote_read`
// supplies the path policy in a remote workspace.
Tool PresentTool(const std::string& session_path,
                 const Tool* remote_read = nullptr);
// write_file's `from`: copies a file attached to this conversation.
void CopyAttachments(std::vector<Tool>& tools, const std::string& session_path);
}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_PRESENT_H_
