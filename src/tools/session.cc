// Copyright 2026 Timon Gentzsch

#include "include/tools/session.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/file_services.h"
#include "include/agent/session_store.h"
#include "include/app/session.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
namespace {

constexpr int kSessionLinkFormat = 1;
constexpr size_t kSessionLinkMembers = 32;
constexpr size_t kSessionLinkFiles = 256;
constexpr int kSessionMailMaxHops = 8;

std::string AutoLinkName() { return "auto-" + HashHex(CanonicalCwd()); }

std::string LinkPath(const std::string& name) {
  return SessionLinkDir() + "/" + name + ".json";
}

bool ValidLinkName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
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
                       "error: bad link name");
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
  if (!ApprovalIsAutomatic()) return ToolSuccess({});
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
                       "error: auto-link is full (32 sessions)");
  }
  members.push_back(std::move(me));
  ToolResult saved = WriteLink(name, members);
  return saved.Ok() ? ToolSuccess({}) : saved;
}

ToolResult CreateSessionLink(std::string& token) {
  json me = OwnMember();
  if (!me.is_object()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: no saved session file yet; say something first "
                       "so the session persists, then link");
  }
  token = session::RandomToken(9);
  if (token.empty() || SafeFileComponent(token) != token) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: cannot mint a link token right now");
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
                       "error: bad link token");
  }
  json link = ReadLink(token);
  if (!link.is_object()) {
    return ToolFailure(ToolErrorCode::kNotFound, "error: unknown link token");
  }
  json me = OwnMember();
  if (!me.is_object()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: no saved session file yet; say something first "
                       "so the session persists, then link");
  }
  json members = JsonValue(link, "members", json::array());
  const std::string id = JsonValue(me, "id", "");
  if (!HasMember(members, id)) {
    if (members.size() >= kSessionLinkMembers) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: link is full (32 sessions)");
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

std::vector<json> SessionSummaries() {
  const std::string me = OwnSessionId();
  std::vector<json> rows;
  auto push = [&](std::string id, std::string title, std::string kind,
                  bool linked) {
    if (id.empty() || id == me) return;
    for (const json& row : rows) {
      if (JsonValue(row, "id", "") == id) return;
    }
    rows.push_back({{"id", std::move(id)},
                    {"title", std::move(title)},
                    {"kind", std::move(kind)},
                    {"linked", linked}});
  };
  // Linked first: members may live in other workspaces ListSessions skips.
  for (const json& member : LinkedMembers()) {
    const std::string id = JsonValue(member, "id", "");
    std::string path = JsonValue(member, "path", "");
    std::string kind = "session";
    if (path.find("/collaborators/") != std::string::npos) {
      kind = "collaborator";
    }
    push(id, MemberTitle(id, path), kind, true);
  }
  // Then linkable workspace sessions.
  for (const SessionInfo& info : ListSessions()) {
    if (!info.error.empty()) continue;
    std::string stem =
        std::filesystem::path(info.path).filename().stem().string();
    push(stem, info.title.empty() ? stem : info.title, "session",
         SharesLink(me, stem));
  }
  // Then same-workspace collaborator children (peer-capable via team mesh,
  // addressable here too).
  std::error_code error;
  for (std::filesystem::directory_iterator
           it(UagentDir("collaborators"), error),
       end;
       !error && it != end; it.increment(error)) {
    std::string name = it->path().filename().string();
    if (!name.ends_with(".json") || name.find(".mail-") != std::string::npos ||
        name.ends_with(".session.json")) {
      continue;
    }
    std::ifstream record(it->path());
    json state = json::parse(record, nullptr, false);
    if (state.is_discarded() || !state.is_object()) continue;
    if (JsonValue(state, "cwd", "") != CanonicalCwd()) continue;
    const std::string id = JsonValue(state, "id", "");
    std::string title = JsonValue(state, "name", "");
    if (title.empty()) title = JsonValue(state, "label", id);
    push(id, title, "collaborator", SharesLink(me, id));
  }
  return rows;
}

ToolResult MessageSession(const std::string& id, const std::string& text,
                          const std::string& from, int hops) {
  const std::string me = OwnSessionId();
  if (me.empty()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: no saved session file yet; say something first "
                       "so the session persists, then message");
  }
  if (id.empty() || SafeFileComponent(id) != id) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: unknown session " + id);
  }
  if (!SharesLink(me, id)) {
    return ToolFailure(
        ToolErrorCode::kPermissionDenied,
        "error: session " + id + " is not linked; join its link first (/link)");
  }
  if (text.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "message requires text");
  }
  if (hops >= kSessionMailMaxHops) {
    return ToolSuccess("dropped message for session " + id + " [loop-clamped]");
  }
  ToolResult saved = WriteSessionMail(id, text, from.empty() ? me : from, hops);
  return saved.Ok() ? ToolSuccess("queued message for session " + id) : saved;
}

Tool SessionTool() {
  json parameters = json{
      {"type", "object"},
      {"properties",
       {{"operation",
         {{"type", "string"},
          {"enum", json::array({"list", "message"})},
          {"description", "list linked/linkable sessions or message one"}}},
        {"session_id",
         {{"type", json::array({"string", "array"})},
          {"items", {{"type", "string"}}},
          {"description",
           "peer session id for message; accepts an array for fan-out"}}},
        {"broadcast",
         {{"type", "boolean"},
          {"description", "message only: fan out to every linked session"}}},
        {"prompt",
         {{"type", "string"},
          {"description", "message text for operation=message"}}},
        {"hops",
         {{"type", "integer"},
          {"minimum", 0},
          {"maximum", 8},
          {"description",
           "message only: peer-forward count for loop clamping"}}}}},
      {"required", json::array({"prompt"})}};
  Tool tool = MakeTool(
      "session",
      "Message another live uagent session (terminal or browser) that shares "
      "a link with this one. Sessions started under yolo auto-link per "
      "workspace; otherwise join with /link TOKEN. list shows linked peers "
      "first, then linkable workspace sessions. message queues text the peer "
      "reads at its next step; broadcast fans out to every linked session. "
      "Delivery is at-least-once and ordered oldest-first; unlinked sessions "
      "reject with a permission error.",
      parameters, [](const json& arguments, const ToolContext&) {
        (void)EnsureSessionAutoLink();
        std::string operation = JsonValue(arguments, "operation", "list");
        if (operation == "list") {
          std::vector<json> peers = SessionSummaries();
          return ToolSuccess(peers.empty() ? "no linked sessions"
                                           : JsonDump(peers, 2));
        }
        if (operation != "message") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: unknown operation " + operation);
        }
        std::string prompt = JsonValue(arguments, "prompt", "");
        std::vector<std::string> targets;
        const json* ids = JsonArray(arguments, "session_id");
        if (ids != nullptr) {
          for (const json& entry : *ids) {
            if (entry.is_string()) targets.push_back(entry.get<std::string>());
          }
        } else {
          std::string single = JsonValue(arguments, "session_id", "");
          if (!single.empty()) targets.push_back(single);
        }
        if (targets.empty() && JsonValue(arguments, "broadcast", false)) {
          for (const json& row : SessionSummaries()) {
            if (JsonValue(row, "linked", false)) {
              targets.push_back(JsonValue(row, "id", ""));
            }
          }
        }
        if (targets.empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: message requires session_id or broadcast");
        }
        const std::string me = OwnSessionId();
        const int hops =
            static_cast<int>(JsonValue(arguments, "hops", int64_t{0}));
        std::string combined;
        for (const std::string& target : targets) {
          ToolResult one = MessageSession(target, prompt, me, hops);
          if (!combined.empty()) combined += "\n";
          combined +=
              one.Ok() ? one.output : ("error " + target + ": " + one.output);
          if (!one.Ok()) return ToolFailure(one.error, combined);
        }
        return ToolSuccess(combined);
      });
  tool.available_in_lean = true;
  return tool;
}

std::string SessionText(const json& result) {
  const json* rows = JsonArray(result, "sessions");
  if (rows == nullptr) return JsonDump(result, 2) + "\n";
  std::string text =
      "sessions (" + FmtCount(static_cast<int64_t>(rows->size())) + ")\n";
  for (const json& row : *rows) {
    const std::string id = JsonValue(row, "id", "");
    std::string title = JsonValue(row, "title", "");
    if (title.empty()) title = id;
    text += (JsonValue(row, "linked", false) ? "" : "[unlinked] ") + title;
    if (title != id) text += " (" + id + ")";
    text += "  " + JsonValue(row, "kind", "session") + "\n";
  }
  return text;
}

json SessionSlashPeers() {
  (void)EnsureSessionAutoLink();
  return {{"sessions", SessionSummaries()}};
}

json SessionSlashTell(const std::string& argument) {
  (void)EnsureSessionAutoLink();
  std::string args = Trim(argument);
  size_t space = args.find_first_of(" \t");
  if (space == std::string::npos) {
    return {{"error", "usage: /tell ID TEXT"}};
  }
  std::string text = Trim(args.substr(space));
  if (text.empty()) return {{"error", "usage: /tell ID TEXT"}};
  ToolResult sent =
      MessageSession(args.substr(0, space), text, OwnSessionId(), 0);
  return sent.Ok() ? json{{"output", sent.output}}
                   : json{{"error", sent.output}};
}

json SessionSlashLink(const std::string& argument) {
  std::string token = Trim(argument);
  if (token.empty()) {
    std::string created;
    ToolResult made = CreateSessionLink(created);
    if (!made.Ok()) return {{"error", made.output}};
    return {{"output", "link token: " + created +
                           "\nhand it to another session as /link " + created}};
  }
  ToolResult joined = JoinSessionLink(token);
  if (!joined.Ok()) return {{"error", joined.output}};
  return {{"output", "joined link " + token}};
}

}  // namespace uagent
