// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SESSION_H_
#define UAGENT_INCLUDE_TOOLS_SESSION_H_
// Session-to-session interaction: linked uagent sessions (terminals,
// browsers, headless runs with a saved file) exchange short text messages
// through one file mailbox, which also carries a parent's guidance to its
// delegated children. A session reaches only its parent, its children and
// its siblings (session_links.h).

#include <functional>
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

// Text-only peer messaging for every toolset, lean included: `session` lists
// the peers, `session_message` writes to them. `start` starts the runtime
// of a panel or thread that a message found stopped; any other session reads
// its mail when it is next opened.
Tool SessionTool();
Tool SessionMessageTool(
    const std::function<void(const std::string& path)>& start = {});

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SESSION_H_
