// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_ARTIFACT_H_
#define UAGENT_INCLUDE_APP_ARTIFACT_H_

#include <string>

#include "include/tools/tool.h"

namespace uagent {
// artifact(path): snapshot a workspace file into the session's asset store so
// the person can open or download it from the conversation.
Tool ArtifactTool(const std::string& session_path);
}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_ARTIFACT_H_
