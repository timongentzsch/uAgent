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
// The yolo auto-link is links/auto-<HashHex(cwd)>.json; token links are
// links/<token>.json. Members are pruned when their session file is gone.
std::string SessionLinkDir();

// True when both ids are named in one link file. The only gate between
// sessions; everything else is addressing.
bool SharesLink(const std::string& a, const std::string& b);

// Join the workspace auto-link. No-op without yolo or without a session
// file, so toggling /yolo mid-session takes effect on the next tool call.
ToolResult EnsureSessionAutoLink();

// Create a token link containing this session; returns the token to hand out.
ToolResult CreateSessionLink(std::string& token);
// Join a token link. Unknown tokens are NotFound, never created implicitly.
ToolResult JoinSessionLink(const std::string& token);

// Linked peers plus linkable workspace sessions:
// [{id,title,linked}]. Self excluded.
std::vector<json> SessionSummaries();

}  // namespace uagent
#endif
