// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
#define UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
// Which sessions may message each other: the workspace's link file, read by
// delegation and the session tool alike.

#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// The yolo auto-link is links/auto-<HashHex(cwd)>.json:
// {format:1, members:[{id,path}]}. Members are pruned when their session file
// is gone.
//
// True when both ids are named in it. The only gate between sessions;
// everything else is addressing.
bool SharesLink(const std::string& a, const std::string& b);
// Whether a session file is a coordinator's or one of its threads'.
bool SocietySession(const std::string& path);

// Join the workspace auto-link. No-op without yolo or without a session
// file, so toggling /yolo mid-session takes effect on the next tool call. A
// coordinator and its threads need no link file: they are linked by sharing
// one history folder.
ToolResult EnsureSessionAutoLink();

// The session file of a session linked with this one, or empty.
std::string LinkedSessionPath(const std::string& id);

// Linked peers plus linkable workspace sessions:
// [{id,title,linked}]. Self excluded.
std::vector<json> SessionSummaries();

}  // namespace uagent
#endif
