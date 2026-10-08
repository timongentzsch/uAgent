// Copyright 2026 Timon Gentzsch

#include "include/agent/session_links.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/file_services.h"
#include "include/agent/session_store.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/transport/session.h"

namespace uagent {

bool SocietySession(const std::string& path) {
  const std::filesystem::path file(path);
  const std::string name = file.filename().string();
  return file.extension() == ".json" &&
         (name == "coordinator.json" || name.starts_with("thread-"));
}

namespace {

constexpr int kSessionLinkFormat = 1;
constexpr size_t kSessionLinkMembers = 32;

// A coordinator and its threads work together, so they are linked by where
// they live, whatever mode each runs in: one history folder holds
// coordinator.json and its thread-*.json. Nothing to join, nothing to prune.
std::vector<json> SocietyMembers() {
  const auto member = SocietySession;
  const std::filesystem::path own(OwnSessionFile());
  std::vector<json> out;
  if (!member(own.string())) return out;
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(own.parent_path(), ec)) {
    if (entry.path() == own || !member(entry.path().string())) continue;
    out.push_back({{"id", entry.path().stem().string()},
                   {"path", entry.path().string()}});
  }
  return out;
}

// The one link file a workspace has.
std::string AutoLinkPath() {
  return UagentDir("links") + "/auto-" + HashHex(CanonicalCwd()) + ".json";
}

// Members whose session file is gone cannot come back; dropping them here
// keeps links from pinning deleted sessions forever.
json PruneMembers(const json& members) {
  json kept = json::array();
  for (const json& member : members) {
    if (!member.is_object()) {
      continue;
    }
    const std::string path = JsonValue(member, "path", "");
    if (!path.empty() && !PathExists(path)) continue;
    kept.push_back(member);
  }
  return kept;
}

ToolResult WriteLink(const json& members) {
  json link = {{"format", kSessionLinkFormat},
               {"members", PruneMembers(members)}};
  return ToolAtomicWrite(AutoLinkPath(), JsonDump(link, 2) + "\n",
                         kPrivateFileMode, /*preserve_mode=*/true);
}

// The reader's own membership card. Empty id (headless runs without a saved
// file) means no link can name this process.
json OwnMember() {
  const std::string id = OwnSessionId();
  if (id.empty()) return json();
  return {{"id", id}, {"path", OwnSessionFile()}};
}

bool HasMember(const json& members, const std::string& id) {
  for (const json& member : members) {
    if (member.is_object() && JsonValue(member, "id", "") == id) return true;
  }
  return false;
}

// Who the link joins. It is for a person's own sessions: a delegated child an
// older version put there is not counted, so it reaches no session its parent
// did not give it. Nor is a session whose saved header cannot be read, which
// could be one.
json LinkMembers() {
  std::ifstream input(AutoLinkPath());
  const json link = json::parse(input, nullptr, false);
  json own = json::array();
  if (JsonValue(link, "format", int64_t{0}) != kSessionLinkFormat) return own;
  for (json& member : JsonValue(link, "members", json::array())) {
    const std::string path = JsonValue(member, "path", "");
    const json header = SessionHeader(path);
    if (!PathExists(path) ||
        (!header.empty() && !header.contains(kSessionHeaderDelegation))) {
      own.push_back(std::move(member));
    }
  }
  return own;
}

std::string MemberTitle(const std::string& id, const std::string& path) {
  if (!path.empty()) {
    std::string title = JsonValue(SessionHeader(path), kSessionHeaderTitle, "");
    if (!title.empty() && title != "(untitled)") return title;
  }
  return id;
}

}  // namespace

bool SharesLink(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty() || a == b) return a == b && !a.empty();
  const json members = LinkMembers();
  if (HasMember(members, a) && HasMember(members, b)) return true;
  const std::string me = OwnSessionId();
  return (me == a || me == b) && HasMember(SocietyMembers(), me == a ? b : a);
}

ToolResult EnsureSessionAutoLink() {
  // A person's own yolo sessions find each other. A delegated child is in
  // yolo only because nobody is there to ask: it reaches no session its
  // parent did not give it.
  if (!ApprovalIsYolo() || AgentDepth() > 0) return ToolSuccess({});
  json me = OwnMember();
  if (!me.is_object()) return ToolSuccess({});
  // Only those who count: the rest take no place and are not saved again.
  json members = LinkMembers();
  const std::string id = JsonValue(me, "id", "");
  if (HasMember(members, id)) return ToolSuccess({});
  if (members.size() >= kSessionLinkMembers) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "auto-link is full (32 sessions)");
  }
  members.push_back(std::move(me));
  ToolResult saved = WriteLink(members);
  return saved.Ok() ? ToolSuccess({}) : saved;
}

namespace {

// Everyone this process is linked with, itself left out.
std::vector<json> LinkedMembers() {
  const std::string me = OwnSessionId();
  std::vector<json> out;
  if (me.empty()) return out;
  if (const json members = LinkMembers(); HasMember(members, me)) {
    for (const json& member : members) {
      if (!member.is_object()) continue;
      const std::string id = JsonValue(member, "id", "");
      if (id.empty() || id == me || HasMember(out, id)) continue;
      out.push_back(member);
    }
  }
  for (json& member : SocietyMembers()) {
    if (!HasMember(out, JsonValue(member, "id", ""))) {
      out.push_back(std::move(member));
    }
  }
  return out;
}

}  // namespace

// A peer is named by the id every board and list shows, the hash of its
// file's path; its file's stem is accepted too.
std::string LinkedSessionPath(const std::string& id) {
  for (const json& member : LinkedMembers()) {
    const std::string path = JsonValue(member, "path", "");
    if (JsonValue(member, "id", "") == id || HashHex(path) == id) return path;
  }
  return "";
}

std::vector<json> SessionSummaries() {
  const std::string me = HashHex(OwnSessionFile());
  std::vector<json> rows;
  // A linked session is already a row by the time the workspace ones arrive.
  auto push = [&](std::string id, std::string title, bool linked) {
    if (id.empty() || id == me) return;
    for (const json& row : rows) {
      if (JsonValue(row, "id", "") == id) return;
    }
    rows.push_back({{"id", std::move(id)},
                    {"title", std::move(title)},
                    {"linked", linked}});
  };
  // Linked first: members may live in other workspaces ListSessions skips.
  for (const json& member : LinkedMembers()) {
    const std::string path = JsonValue(member, "path", "");
    push(HashHex(path), MemberTitle(JsonValue(member, "id", ""), path), true);
  }
  // Then linkable workspace sessions.
  for (const SessionInfo& info : ListSessions()) {
    if (!info.error.empty()) continue;
    push(HashHex(info.path),
         info.title.empty()
             ? std::filesystem::path(info.path).filename().stem().string()
             : info.title,
         false);
  }
  return rows;
}

}  // namespace uagent
