// Copyright 2026 Timon Gentzsch

#include "include/agent/session_links.h"

#include <sys/stat.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/mailbox.h"
#include "include/core/strings.h"
#include "include/transport/session.h"

namespace uagent {

bool SocietySession(const std::string& path) {
  const std::filesystem::path file(path);
  const std::string name = file.filename().string();
  return file.extension() == ".json" &&
         (name == kPanelRecordFile || name.starts_with("thread-"));
}

MailParty SessionParty(const std::string& path) {
  const json header = SessionHeader(path);
  MailParty party{.id = MailboxIdFor(path),
                  .path = path,
                  .cwd = JsonValue(header, kSessionHeaderCwd, ""),
                  .delegation = DelegationOf(header)};
  if (party.delegation.empty() && path == OwnSessionFile()) {
    party.delegation = OwnDelegation();
  }
  return party;
}

MailParty OwnerParty(const std::string& owner_id) { return {.id = owner_id}; }

namespace {

std::string ParentSession(const MailParty& party) {
  return MailboxIdFor(JsonValue(party.delegation, "parent_session", ""));
}

bool NamesParent(const MailParty& child, const std::string& id) {
  return !id.empty() && (id == ParentSession(child) ||
                         id == JsonValue(child.delegation, "parent", ""));
}

// A panel and its threads work together, so they are linked by where they
// live: one history folder holds panel.json and its thread-*.json.
bool SameSociety(const MailParty& a, const MailParty& b) {
  return SocietySession(a.path) && SocietySession(b.path) &&
         std::filesystem::path(a.path).parent_path() ==
             std::filesystem::path(b.path).parent_path();
}

// A person's own sessions in one folder, while this process is in yolo. A
// delegated session is in yolo only because nobody is there to ask, so it
// reaches no session its parent did not give it.
bool YoloPeers(const MailParty& a, const MailParty& b) {
  return ApprovalIsYolo() && AgentDepth() == 0 && !a.path.empty() &&
         !b.path.empty() && a.delegation.empty() && b.delegation.empty() &&
         !a.cwd.empty() && a.cwd == b.cwd;
}

// Whether a runtime is serving the session now.
bool Live(const std::string& path) {
  struct stat info{};
  return stat(session::SocketPathForId(SessionIdFor(path)).c_str(), &info) == 0;
}

// Everyone this process may choose to message, itself left out: its
// society, then the live yolo peers in its folder.
std::vector<json> LinkedMembers() {
  const std::string own = OwnSessionFile();
  std::vector<json> out;
  if (own.empty()) return out;
  const MailParty me = SessionParty(own);
  const auto add = [&](const std::string& path) {
    out.push_back(
        {{"id", std::filesystem::path(path).stem().string()}, {"path", path}});
  };
  if (SocietySession(own)) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(own).parent_path(), ec)) {
      if (entry.path() != own && SocietySession(entry.path().string())) {
        add(entry.path().string());
      }
    }
  }
  for (const SessionInfo& info : ListSessions()) {
    if (info.error.empty() && info.path != own && Live(info.path) &&
        YoloPeers(me, {.path = info.path,
                       .cwd = info.cwd,
                       .delegation = info.delegation})) {
      add(info.path);
    }
  }
  return out;
}

std::string MemberTitle(const std::string& id, const std::string& path) {
  std::string title = JsonValue(SessionHeader(path), kSessionHeaderTitle, "");
  return title.empty() || title == "(untitled)" ? id : title;
}

}  // namespace

bool MayMail(const MailParty& from, const MailParty& to) {
  if (NamesParent(to, from.id) || NamesParent(from, to.id)) return true;
  const std::string parent = ParentSession(from);
  if (!parent.empty() && parent == ParentSession(to)) return true;
  return SameSociety(from, to) || YoloPeers(from, to);
}

std::string SendSessionMail(SessionMail mail) {
  const MailParty from = mail.from_path.empty() ? OwnerParty(mail.from_owner)
                                                : SessionParty(mail.from_path);
  MailParty to = SessionParty(mail.to_path);
  if (to.delegation.empty()) to.delegation = mail.to_delegation;
  if (!MayMail(from, to)) {
    return "not allowed to message that session";
  }
  if (mail.kind == MailKind::kGuidance) {
    mail.text = "[parent guidance]\n" + mail.text;
  } else if (mail.kind == MailKind::kPeer) {
    // Named as lists and boards name it, so the reply can address it.
    const std::string title =
        JsonValue(SessionHeader(mail.from_path), kSessionHeaderTitle, "");
    const std::string name = HashHex(mail.from_path);
    mail.text = "[session " +
                (title.empty() ? name : OneLine(title) + " (" + name + ")") +
                "]\n" + mail.text;
  }
  Mail sent = MailFrom(mail.from_path, MailboxIdFor(mail.to_path),
                       {{"text", std::move(mail.text)}});
  if (mail.from_path.empty()) sent.from = from.id;
  sent.id = std::move(mail.id);
  sent.hops = mail.hops;
  // What a thread reports to its panel is the panel's to hear, however many;
  // the rate is for what sessions write to each other.
  return SendMail(std::move(sent), mail.kind != MailKind::kEvent);
}

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
  for (const json& member : LinkedMembers()) {
    const std::string path = JsonValue(member, "path", "");
    push(HashHex(path), MemberTitle(JsonValue(member, "id", ""), path), true);
  }
  // Then the workspace's other sessions, which can be read but not messaged.
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
