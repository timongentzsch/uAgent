// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
#define UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
// Which sessions may message each other, and the one way to message one. A
// message is a Mail (core/mailbox.h); this decides who may send it and how
// its text names the sender.

#include <string>

#include "include/core/json.h"

namespace uagent {

// A session as the mail rule sees it.
struct MailParty {
  std::string id{};      // mailbox id (MailboxIdFor path)
  std::string path{};    // its session file
  std::string parent{};  // its parent's mailbox id, for an agent
};

// A saved session by its file and its header's agent role, or by `role`
// (AgentOf) when its header is not saved yet.
MailParty SessionParty(const std::string& path);
MailParty SessionParty(const std::string& path, const json& role);

// Whether `from` may message `to`: its parent, its child, or a sibling under
// one parent.
bool MayMail(const MailParty& from, const MailParty& to);

// Every message wakes an idle recipient, labelled by how its sender stands
// to the recipient.
struct SessionMail {
  std::string from_path{};  // the sender's session file
  std::string to_path{};
  std::string text{};
  std::string id{};  // set to send the same message again as itself
  int hops = 0;
  // An agent's report to its parent: it names its sender itself, and is
  // owed however many, so it is neither labelled nor rate limited. `state`
  // says what it reports, for a parent waiting on it: the turn's stop
  // reason, or "needs decision".
  bool report = false;
  std::string state{};
};

// Sends it if the rule allows; returns why not, or empty. Waking a recipient
// that has no runtime is the caller's, which knows how to start one.
std::string SendSessionMail(SessionMail mail);

}  // namespace uagent
#endif
