// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_COORDINATOR_H_
#define UAGENT_INCLUDE_APP_COORDINATOR_H_
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "include/agent/session_store.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {
// The session id of a folder's coordinator, as clients and threads name it.
std::string CoordinatorId(const std::string& folder);

// A thread's spending ceiling in USD; 0 when it has none.
double ThreadBudget(const json& thread);

// Every session the coordinator of `folder` manages: those opened in the
// folder and the threads it launched elsewhere (worktrees), newest first.
std::vector<SessionInfo> FolderSessions(const std::string& folder);

// The threads this folder's coordinator started, chat members included.
std::vector<SessionInfo> OwnThreads(const std::string& folder);

// A JSON object the coordinator keeps beside its session file, under
// `suffix`: its pinned notes, its chat's round. Not named *.json, so the
// session catalogue never mistakes one for a session. Reading gives an
// empty object when there is none; writing returns why it failed.
json ReadCoordinatorFile(const std::string& folder, const char* suffix);
std::string WriteCoordinatorFile(const std::string& folder, const char* suffix,
                                 const json& value);

// One line per managed session, newest first, bounded to kBoardBytes. Rendered
// fresh on each request; never stored.
std::string CoordinatorBoard(const std::string& folder);

// What a coordinator sees fresh every turn: the time, its pinned notes and
// the board. Outside the transcript, so compaction never loses it.
std::string CoordinatorContext(const std::string& folder);

// Records the coordinator's session cost before each of its requests; the
// day's first sets the baseline its own daily spend is measured from.
void RecordCoordinatorCost(const std::string& folder, double cost);

// Why the coordinator holds its mail, or empty: today's spend, its own
// turns and its threads', has reached the daily limit.
std::string CoordinatorPause(const std::string& folder);

// Everything a coordinator may use: it reads and delegates, never writes or
// runs, whatever the capability setting allows.
inline constexpr const char* kCoordinatorTools[] = {
    "read_path", "grep",   "memory", "skill", "uagent",
    "history",   "thread", "decide", "state", "ask"};

// Everything a member of the coordinator's chat may use: it reads and
// searches to check what it says, and changes nothing.
inline constexpr const char* kMemberTools[] = {"read_path", "grep", "skill",
                                               "web_search", "web_fetch"};

// Whether a thread this folder's coordinator started is working a turn now.
bool ThreadsOwe(const std::string& folder);
// Whether something can still bring the answer of a chat member that was
// woken: a runtime that does not wait on a person, or a wake-up sent moments
// ago that is starting one.
bool AnswerAhead(const SessionInfo& member);

// `own_model` names the model the coordinator is on right now, as a
// selection a thread can be started with.
void AddCoordinatorTools(std::vector<Tool>& tools, const std::string& folder,
                         std::function<std::string()> own_model);
}  // namespace uagent
#endif
