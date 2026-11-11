// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_PRESENT_H_
#define UAGENT_INCLUDE_APP_PRESENT_H_
// The present tool: hands the person what the agent made.

#include <string>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {
// present(path): an artifact's folder or page (document, deck, design, design
// system) is shown from the folder, where it stays live; any other file is
// snapshotted into the session's asset store to open or download. In a
// remote workspace its path policy is that of the forwarded read_path,
// `remote_read`.
Tool PresentTool(const std::string& session_path,
                 const Tool* remote_read = nullptr);
// write_file's `from`: copy a file attached to this conversation, read from
// its own store, into the folder.
void CopyAttachments(std::vector<Tool>& tools, const std::string& session_path);
}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_PRESENT_H_
