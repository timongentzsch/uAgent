// Copyright 2026 Timon Gentzsch

#include "include/agent/session_links.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/core/json.h"
#include "include/core/mailbox.h"
#include "include/core/strings.h"

namespace uagent {

MailParty SessionParty(const std::string& path) {
  MailParty party{.id = MailboxIdFor(path),
                  .path = path,
                  .delegation = DelegationOf(SessionHeader(path))};
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

// A sender named as lists and boards name it, so the reply can address it.
std::string SessionLabel(const std::string& path) {
  const std::string title =
      JsonValue(SessionHeader(path), kSessionHeaderTitle, "");
  const std::string name = HashHex(path);
  return "[session " +
         (title.empty() ? name : OneLine(title) + " (" + name + ")") + "]";
}

// Every saved session this process may message, itself left out.
std::vector<SessionInfo> LinkedSessions() {
  const std::string own = OwnSessionFile();
  if (own.empty()) return {};
  const MailParty me = SessionParty(own);
  std::vector<SessionInfo> linked = ListSessions(SessionScope::kAll);
  std::erase_if(linked, [&](const SessionInfo& info) {
    return !info.error.empty() || info.path == own ||
           !MayMail(me, {.id = MailboxIdFor(info.path),
                         .path = info.path,
                         .delegation = info.delegation});
  });
  return linked;
}

}  // namespace

bool MayMail(const MailParty& from, const MailParty& to) {
  if (NamesParent(to, from.id) || NamesParent(from, to.id)) return true;
  const std::string parent = ParentSession(from);
  return !parent.empty() && parent == ParentSession(to);
}

std::string SendSessionMail(SessionMail mail) {
  const MailParty from = mail.from_path.empty() ? OwnerParty(mail.from_owner)
                                                : SessionParty(mail.from_path);
  MailParty to = SessionParty(mail.to_path);
  if (to.delegation.empty()) to.delegation = mail.to_delegation;
  if (!MayMail(from, to)) {
    return "not allowed to message that session";
  }
  // The label says who wrote: the recipient's parent, or a session it can
  // answer by name. A runtime's report names its sender itself.
  if (!mail.report) {
    mail.text = (NamesParent(to, from.id) ? std::string("[parent guidance]")
                                          : SessionLabel(mail.from_path)) +
                "\n" + mail.text;
  }
  Mail sent = MailFrom(mail.from_path, MailboxIdFor(mail.to_path),
                       {{"text", std::move(mail.text)}});
  if (mail.from_path.empty()) sent.from = from.id;
  sent.id = std::move(mail.id);
  sent.hops = mail.hops;
  // What a runtime reports to its parent is the parent's to hear, however
  // many; the rate is for what sessions write to each other.
  return SendMail(std::move(sent), !mail.report);
}

// A session is named by the id every board and list shows, the hash of its
// file's path; its file's stem is accepted too.
std::string LinkedSessionPath(const std::string& id) {
  for (const SessionInfo& info : LinkedSessions()) {
    if (std::filesystem::path(info.path).stem() == id ||
        HashHex(info.path) == id) {
      return info.path;
    }
  }
  return "";
}

std::vector<json> SessionSummaries() {
  const std::string me = HashHex(OwnSessionFile());
  std::vector<json> rows;
  // A linked session is already a row by the time the folder's others arrive.
  auto push = [&](const SessionInfo& info, bool linked) {
    const std::string id = HashHex(info.path);
    if (id == me) return;
    for (const json& row : rows) {
      if (JsonValue(row, "id", "") == id) return;
    }
    rows.push_back(
        {{"id", id},
         {"title", info.title.empty()
                       ? std::filesystem::path(info.path).stem().string()
                       : info.title},
         {"linked", linked}});
  };
  for (const SessionInfo& info : LinkedSessions()) push(info, true);
  // Then the folder's other sessions, which can be read but not messaged.
  for (const SessionInfo& info : ListSessions()) {
    if (info.error.empty()) push(info, false);
  }
  return rows;
}

}  // namespace uagent
