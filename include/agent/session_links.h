// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
#define UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
// Which sessions may message each other: the link files, read by delegation
// and the session tool alike.

#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// Link files live at links/<name>.json: {format:1, members:[{id,path}]}.
// The yolo auto-link is links/auto-<HashHex(cwd)>.json. Members are pruned
// when their session file is gone.
std::string SessionLinkDir();

// True when both ids are named in one link file. The only gate between
// sessions; everything else is addressing.
bool SharesLink(const std::string& a, const std::string& b);

// Join the workspace auto-link. No-op without a session file, and for a
// session that is neither under yolo nor a coordinator's thread, so toggling
// /yolo mid-session takes effect on the next tool call.
ToolResult EnsureSessionAutoLink();

// The session file of a session linked with this one, or empty.
std::string LinkedSessionPath(const std::string& id);

// Linked peers plus linkable workspace sessions:
// [{id,title,linked}]. Self excluded.
std::vector<json> SessionSummaries();

}  // namespace uagent
#endif
