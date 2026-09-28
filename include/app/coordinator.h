// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_COORDINATOR_H_
#define UAGENT_INCLUDE_APP_COORDINATOR_H_
#include <string>
#include <vector>

#include "include/agent/session_store.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {
// The session id of a folder's coordinator, as clients and threads name it.
std::string CoordinatorId(const std::string& folder);

// Every session the coordinator of `folder` manages: those opened in the
// folder and the threads it launched elsewhere (worktrees), newest first.
std::vector<SessionInfo> FolderSessions(const std::string& folder);

// One line per managed session, newest first, bounded to kBoardBytes. Rendered
// fresh on each request; never stored.
std::string CoordinatorBoard(const std::string& folder);

// The tools only a folder's coordinator gets.
void AddCoordinatorTools(std::vector<Tool>& tools, const std::string& folder);
}  // namespace uagent
#endif
