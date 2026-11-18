// Copyright 2026 Timon Gentzsch

#include "include/agent/session_links.h"

#include <string>
#include <utility>

#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/core/json.h"
#include "include/core/mailbox.h"
#include "include/core/strings.h"

namespace uagent {

MailParty SessionParty(const std::string& path) {
  return SessionParty(path, AgentOf(SessionHeader(path)));
}

MailParty SessionParty(const std::string& path, const json& role) {
  const json header = SessionHeader(path);
  MailParty party{.id = MailboxIdFor(path),
                  .path = path,
                  .identity = JsonValue(header, kSessionHeaderSessionId, ""),
                  .source = JsonValue(header, kSessionHeaderParent, "")};
  const std::string parent =
      AgentParentPath(path, JsonValue(role, "parent", ""));
  if (!parent.empty()) party.parent = MailboxIdFor(parent);
  return party;
}

namespace {
// A sender named as lists name it, so the reply can address it.
std::string SessionLabel(const std::string& path) {
  const std::string title =
      JsonValue(SessionHeader(path), kSessionHeaderTitle, "");
  const std::string name = SessionIdFor(path);
  return "[session " +
         (title.empty() ? name : OneLine(title) + " (" + name + ")") + "]";
}
}  // namespace

bool MayMail(const MailParty& from, const MailParty& to, bool report) {
  return (!to.parent.empty() && to.parent == from.id) ||
         (!from.parent.empty() &&
          (from.parent == to.id || from.parent == to.parent)) ||
         // A side chat the person merges reports to where it branched from.
         (report && !from.source.empty() && from.source == to.identity);
}

std::string SendSessionMail(SessionMail mail) {
  const MailParty from = SessionParty(mail.from_path);
  const MailParty to = SessionParty(mail.to_path);
  if (!MayMail(from, to, mail.report)) {
    return "not allowed to message that session";
  }
  // The label says who wrote: the recipient's parent, or a session it can
  // answer by name. An agent's report names its sender itself.
  if (!mail.report) {
    mail.text = (to.parent == from.id ? std::string("[parent guidance]")
                                      : SessionLabel(mail.from_path)) +
                "\n" + mail.text;
  }
  json body = {{"text", std::move(mail.text)}};
  if (!mail.state.empty()) body["state"] = std::move(mail.state);
  if (!mail.agent.is_null()) body["agent"] = std::move(mail.agent);
  Mail sent =
      MailFrom(mail.from_path, MailboxIdFor(mail.to_path), std::move(body));
  sent.id = std::move(mail.id);
  sent.hops = mail.hops;
  // What an agent reports to its parent is the parent's to hear, however
  // many; the rate is for what sessions write to each other.
  return SendMail(std::move(sent), !mail.report);
}

}  // namespace uagent
