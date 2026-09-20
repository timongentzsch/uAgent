// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SESSION_H_
#define UAGENT_INCLUDE_TOOLS_SESSION_H_
// Session-to-session interaction: linked uagent sessions (terminals,
// browsers, headless runs with a saved file) exchange short text messages
// through the same file-mailbox posture as collaborator guidance. Isolation
// is the default: two sessions read each other's mail only while a link file
// names them both. Sessions started under yolo auto-join the workspace link;
// everyone else joins with a token from /link.

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
// [{id,title,kind(session|collaborator),linked}]. Self excluded.
std::vector<json> SessionSummaries();

// One recipient. Gate first, then mail-first delivery like MessageCollaborator
// but without the socket ping: sessions in other processes have no shared
// runtime to ping, so the next step boundary is the delivery point.
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
