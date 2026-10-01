// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SESSION_H_
#define UAGENT_INCLUDE_TOOLS_SESSION_H_
// Session-to-session interaction: linked uagent sessions (terminals,
// browsers, headless runs with a saved file) exchange short text messages
// through one file mailbox, which also carries a parent's guidance to its
// delegated children. Isolation
// is the default: two sessions read each other's mail only while a link file
// names them both. Sessions started under yolo auto-join the workspace link.

#include <string>
#include <vector>

#include "include/agent/session_links.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// One linked recipient, through its mailbox (core/mailbox.h): a running
// session reads it at its next step, an idle one starts a turn on it.
ToolResult MessageSession(const std::string& id, const std::string& text,
                          int hops = 0);

// Text-only peer messaging for every toolset, lean included.
Tool SessionTool();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SESSION_H_
