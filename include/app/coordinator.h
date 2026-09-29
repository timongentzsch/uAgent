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

// What a coordinator sees fresh every turn: the time, its pinned notes and
// the board. Outside the transcript, so compaction never loses it.
std::string CoordinatorContext(const std::string& folder);

// Records the coordinator's session cost before each of its requests; the
// day's first sets the baseline its own daily spend is measured from.
void RecordCoordinatorCost(const std::string& folder, double cost);

// Why the coordinator holds thread events, or empty: today's spend, its own
// turns and its threads', has reached the daily limit.
std::string CoordinatorPause(const std::string& folder);

// Everything a coordinator may use: it reads and delegates, never writes or
// runs, whatever the capability setting allows.
inline constexpr const char* kCoordinatorTools[] = {
    "read_path", "grep",   "memory", "skill", "uagent",
    "history",   "thread", "decide", "state", "ask"};

// The tools only a folder's coordinator gets.
void AddCoordinatorTools(std::vector<Tool>& tools, const std::string& folder);
}  // namespace uagent
#endif
