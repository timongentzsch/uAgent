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
namespace {

constexpr int kSessionLinkFormat = 1;
constexpr size_t kSessionLinkMembers = 32;
constexpr size_t kSessionLinkFiles = 256;
constexpr size_t kSessionLinkNameChars = 64;

std::string AutoLinkName() { return "auto-" + HashHex(CanonicalCwd()); }

std::string LinkPath(const std::string& name) {
  return SessionLinkDir() + "/" + name + ".json";
}

bool ValidLinkName(const std::string& name) {
  if (name.empty() || name.size() > kSessionLinkNameChars) return false;
  return SafeFileComponent(name) == name;
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

json ReadLink(const std::string& name) {
  if (!ValidLinkName(name)) return json::object();
  std::ifstream input(LinkPath(name));
  // Misses come back null, never an empty object: an empty object would
  // read as a link with no members and let unknown tokens create links.
  json link = json::parse(input, nullptr, false);
  if (!link.is_object()) return json();
  if (JsonValue(link, "format", int64_t{0}) != kSessionLinkFormat) {
    return json();
  }
  return link;
}

ToolResult WriteLink(const std::string& name, const json& members) {
  if (!ValidLinkName(name)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "bad link name");
  }
  json link = {{"format", kSessionLinkFormat},
               {"members", PruneMembers(members)}};
  return ToolAtomicWrite(LinkPath(name), JsonDump(link, 2) + "\n",
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

std::vector<std::string> LinkFiles() {
  std::vector<std::string> names;
  std::error_code error;
  for (std::filesystem::directory_iterator it(SessionLinkDir(), error), end;
       !error && it != end; it.increment(error)) {
    if (names.size() >= kSessionLinkFiles) break;
    std::string name = it->path().filename().string();
    if (name.ends_with(".json") && it->is_regular_file(error)) {
      name.resize(name.size() - 5);
      if (ValidLinkName(name)) names.push_back(name);
    }
  }
  return names;
}

std::string MemberTitle(const std::string& id, const std::string& path) {
  if (!path.empty()) {
    auto loaded = SessionStore::Inspect(path);
    if (loaded.record && !loaded.record->metadata.title.empty() &&
        loaded.record->metadata.title != "(untitled)") {
      return loaded.record->metadata.title;
    }
  }
  return id;
}

}  // namespace

std::string SessionLinkDir() { return UagentDir("links"); }

bool SharesLink(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty() || a == b) return a == b && !a.empty();
  for (const std::string& name : LinkFiles()) {
    json link = ReadLink(name);
    if (!link.is_object()) continue;
    const json members = JsonValue(link, "members", json::array());
    if (HasMember(members, a) && HasMember(members, b)) return true;
  }
  return false;
}

ToolResult EnsureSessionAutoLink() {
  if (!ApprovalIsYolo()) return ToolSuccess({});
  json me = OwnMember();
  if (!me.is_object()) return ToolSuccess({});
  const std::string name = AutoLinkName();
  json link = ReadLink(name);
  json members = !link.is_object() ? json::array()
                                   : JsonValue(link, "members", json::array());
  const std::string id = JsonValue(me, "id", "");
  if (HasMember(members, id)) return ToolSuccess({});
  if (members.size() >= kSessionLinkMembers) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "auto-link is full (32 sessions)");
  }
  members.push_back(std::move(me));
  ToolResult saved = WriteLink(name, members);
  return saved.Ok() ? ToolSuccess({}) : saved;
}

ToolResult CreateSessionLink(std::string& token) {
  json me = OwnMember();
  if (!me.is_object()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "no saved session file yet; say something first "
                       "so the session persists, then link");
  }
  token = session::RandomToken(9);
  if (token.empty() || SafeFileComponent(token) != token) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "cannot mint a link token right now");
  }
  json members = json::array();
  members.push_back(std::move(me));
  ToolResult saved = WriteLink(token, members);
  if (!saved.Ok()) return saved;
  return ToolSuccess(token);
}

ToolResult JoinSessionLink(const std::string& token) {
  if (!ValidLinkName(token)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "bad link token");
  }
  json link = ReadLink(token);
  if (!link.is_object()) {
    return ToolFailure(ToolErrorCode::kNotFound, "unknown link token");
  }
  json me = OwnMember();
  if (!me.is_object()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "no saved session file yet; say something first "
                       "so the session persists, then link");
  }
  json members = JsonValue(link, "members", json::array());
  const std::string id = JsonValue(me, "id", "");
  if (!HasMember(members, id)) {
    if (members.size() >= kSessionLinkMembers) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "link is full (32 sessions)");
    }
    members.push_back(std::move(me));
    ToolResult saved = WriteLink(token, members);
    if (!saved.Ok()) return saved;
  }
  return ToolSuccess(token);
}

namespace {

// All ids this process shares any link with, including itself.
std::vector<json> LinkedMembers() {
  const std::string me = OwnSessionId();
  std::vector<json> out;
  if (me.empty()) return out;
  for (const std::string& name : LinkFiles()) {
    json link = ReadLink(name);
    if (!link.is_object()) continue;
    const json members = JsonValue(link, "members", json::array());
    if (!HasMember(members, me)) continue;
    for (const json& member : members) {
      if (!member.is_object()) continue;
      const std::string id = JsonValue(member, "id", "");
      if (id.empty() || id == me) continue;
      if (HasMember(out, id)) continue;
      out.push_back(member);
    }
  }
  return out;
}

}  // namespace

std::string LinkedSessionPath(const std::string& id) {
  for (const json& member : LinkedMembers()) {
    if (JsonValue(member, "id", "") == id) {
      return JsonValue(member, "path", "");
    }
  }
  return "";
}

std::vector<json> SessionSummaries() {
  const std::string me = OwnSessionId();
  std::vector<json> rows;
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
    const std::string id = JsonValue(member, "id", "");
    push(id, MemberTitle(id, JsonValue(member, "path", "")), true);
  }
  // Then linkable workspace sessions.
  for (const SessionInfo& info : ListSessions()) {
    if (!info.error.empty()) continue;
    std::string stem =
        std::filesystem::path(info.path).filename().stem().string();
    push(stem, info.title.empty() ? stem : info.title, SharesLink(me, stem));
  }
  return rows;
}

}  // namespace uagent
