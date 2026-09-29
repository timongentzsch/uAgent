// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SESSION_H_
#define UAGENT_INCLUDE_TOOLS_SESSION_H_
// Session-to-session interaction: linked uagent sessions (terminals,
// browsers, headless runs with a saved file) exchange short text messages
// through one file mailbox, which also carries a parent's guidance to its
// delegated children. Isolation
// is the default: two sessions read each other's mail only while a link file
// names them both. Sessions started under yolo auto-join the workspace link;
// everyone else joins with a token from /link.

#include <string>
#include <vector>

#include "include/agent/session_links.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// One recipient. Gate first, then mail-first delivery: the next step boundary
// is the delivery point.
ToolResult MessageSession(const std::string& id, const std::string& text,
                          const std::string& from = "", int hops = 0);

// Text-only peer messaging for every toolset, lean included.
Tool SessionTool();

// Terminal rendering for /peers: subagent-style rows with names first.
std::string SessionText(const json& result);

// /peers, /tell, /link handlers. Print-ready {output}/{error} objects;
// the dispatcher prints them like ActivityCommand results.
json SessionSlashPeers();
json SessionSlashTell(const std::string& argument);
json SessionSlashLink(const std::string& argument);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SESSION_H_
