// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_ARTIFACT_H_
#define UAGENT_INCLUDE_APP_ARTIFACT_H_
// The artifact tool: shares a file from the folder with the person.

#include <string>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {
// artifact(path): snapshot a folder file into the session's asset store so
// the person can open or download it from the conversation. In a remote
// workspace its path policy is that of the forwarded read_path, `remote_read`.
Tool ArtifactTool(const std::string& session_path,
                  const Tool* remote_read = nullptr);
// write_file's `from`: copy a file attached to this conversation, read from
// its own store, into the folder.
void CopyAttachments(std::vector<Tool>& tools, const std::string& session_path);
}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_ARTIFACT_H_
