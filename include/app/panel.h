// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_PANEL_H_
#define UAGENT_INCLUDE_APP_PANEL_H_
// A folder's panel: its id, threads, budgets and files. Used by the
// session host and the panel tools.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "include/agent/session_store.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {
// The session id of a folder's panel, as clients and threads name it.
std::string PanelId(const std::string& folder);

// A thread's spending ceiling in USD; 0 when it has none.
double ThreadBudget(const json& thread);

// Every session the panel of `folder` manages: those opened in the
// folder and the threads it launched elsewhere (worktrees), newest first.
std::vector<SessionInfo> FolderSessions(const std::string& folder);

// The threads this folder's panel started.
std::vector<SessionInfo> OwnThreads(const std::string& folder);

// A JSON object the panel keeps beside its session file, under
// `suffix`: its spend. Not named *.json, so the
// session catalogue never mistakes one for a session. Reading gives an
// empty object when there is none; writing returns why it failed.
json ReadPanelFile(const std::string& folder, const char* suffix);
std::string WritePanelFile(const std::string& folder, const char* suffix,
                           const json& value);

// One line per managed session, newest first, bounded to kBoardBytes. Rendered
// fresh on each request; never stored.
std::string PanelBoard(const std::string& folder);

// What a panel sees fresh every turn: the time and the board. Outside the
// transcript, so compaction never loses it.
std::string PanelContext(const std::string& folder);

// Records the panel's session cost before each of its requests; the
// day's first sets the baseline its own daily spend is measured from.
void RecordPanelCost(const std::string& folder, double cost);

// Why the panel holds its mail, or empty: today's spend, its own
// turns and its threads', has reached the daily limit.
std::string PanelPause(const std::string& folder);

// The lead agent reads, researches, delegates and shares completed files. It
// cannot edit folder files, whatever the capability setting allows, and
// its commands run in the readonly sandbox (session_worker.cc); neither can
// its subagents: a child gets only its parent's tools and sandbox.
inline constexpr const char* kPanelTools[] = {
    "read_path",
    "grep",
    "run",
    "memory",
    "skill",
    "uagent",
    "uagent_configure",
    "uagent_instructions",
    "history",
    "thread_spawn",
    "thread",
    "decide",
    "plan",
    "ask",
    "browser",
    "artifact",
    "web_search",
    "web_fetch",
    "subagent",
    "subagent_fanout",
};

// Whether anything the panel waits for is still ahead: a thread working or
// not begun, or mail from a thread not yet read or acknowledged. What a
// headless panel run waits for.
bool ThreadsOwe(const std::string& folder);

// `own_model` names the model the panel is on right now, as a
// selection a thread can be started with.
void AddPanelTools(std::vector<Tool>& tools, const std::string& folder,
                   std::function<std::string()> own_model);
}  // namespace uagent
#endif
