// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_PANEL_H_
#define UAGENT_INCLUDE_APP_PANEL_H_
// A folder's panel: its board, its daily spend and its tools. Used by the
// session host and the panel's runtime.

#include <string>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {
// One line per conversation of the folder, newest first, bounded to
// kBoardBytes. Rendered fresh on each request; never stored.
std::string PanelBoard(const std::string& folder);

// What a panel sees fresh every turn: the time and the board. Outside the
// transcript, so compaction never loses it.
std::string PanelContext(const std::string& folder);

// Records the panel's session cost before each of its requests; the
// day's first sets the baseline its own daily spend is measured from.
void RecordPanelCost(const std::string& folder, double cost);

// Why the panel holds its mail, or empty: today's spend, its own turns
// and what its agents reported into them, has reached the daily limit.
std::string PanelPause(const std::string& folder);

// The lead agent reads, researches, delegates and shares completed files. It
// cannot edit folder files, whatever the capability setting allows, and
// its commands run in the readonly sandbox (session_worker.cc).
inline constexpr const char* kPanelTools[] = {
    "read_path",
    "grep",
    "run",
    "memory",
    "skill",
    "uagent",
    "uagent_configure",
    "uagent_instructions",
    "agent_start",
    "agent_send",
    "agents",
    "decide",
    "todo",
    "ask",
    "browser",
    "artifact",
    "web_search",
    "web_fetch",
};

// The panel keeps what it learns about you without being asked.
void AdaptPanelTools(std::vector<Tool>& tools);
}  // namespace uagent
#endif
