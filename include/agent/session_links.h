// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
#define UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
// Which sessions may message each other, and the one way to message one:
// who may send a Mail (core/mailbox.h) and how its text names the sender.

#include <string>

#include "include/core/json.h"

namespace uagent {

struct MailParty {
  std::string id{};  // mailbox id (MailboxIdFor path)
  std::string path{};
  std::string parent{};  // its parent's mailbox id, for an agent
};

// With `role` (AgentOf) for a session whose header is not saved yet.
MailParty SessionParty(const std::string& path);
MailParty SessionParty(const std::string& path, const json& role);

// True for its parent, its child, or a sibling under one parent.
bool MayMail(const MailParty& from, const MailParty& to);

// Wakes an idle recipient, labelled by how its sender stands to it.
struct SessionMail {
  std::string from_path{};
  std::string to_path{};
  std::string text{};
  std::string id{};  // set to send the same message again as itself
  int hops = 0;
  // An agent's report to its parent: neither labelled nor rate limited.
  bool report = false;
  // What a report reports: the turn's stop reason, or "needs decision".
  std::string state{};
  // A finished agent's report as facts: {session, title, outcome}.
  json agent = nullptr;
};

// Why it was refused, or empty. Starting a recipient's runtime is the caller's.
std::string SendSessionMail(SessionMail mail);

}  // namespace uagent
#endif
