// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
#define UAGENT_INCLUDE_AGENT_SESSION_LINKS_H_
// Which sessions may message each other, and the one way to message one. A
// message is a Mail (core/mailbox.h); this decides who may send it and how
// its text names the sender.

#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

// A session as the mail rule sees it. A parent that has no session file is
// known to its child only by its owner id.
struct MailParty {
  std::string id{};    // mailbox id (MailboxIdFor path), or the owner id
  std::string path{};  // empty for an owner id
  std::string cwd{};   // the folder it works in
  json delegation = json::object();  // the role it was given, if any
};

// A saved session by its file: header role and folder. A header not saved
// yet falls back to the role this process was started with.
MailParty SessionParty(const std::string& path);
MailParty OwnerParty(const std::string& owner_id);

// Whether `from` may message `to`: its parent, its child, a sibling under
// one parent, a member of one panel's society (the panel and its threads, in
// one history folder), or a yolo peer. The last needs this process in yolo
// at depth 0 and both sessions undelegated in one folder; each side checks
// its own mode, the sender when sending and the recipient when taking.
bool MayMail(const MailParty& from, const MailParty& to);

// Whether a session file is a panel's or one of its threads'.
bool SocietySession(const std::string& path);

// How the recipient's model is told who wrote: guidance from its parent,
// a note from a peer, or an event (the sender's text is already marked).
// Every kind wakes an idle recipient; the kind only picks the label.
enum class MailKind { kGuidance, kPeer, kEvent };

struct SessionMail {
  std::string from_path{};   // the sender's session file
  std::string from_owner{};  // its owner id when it has no session file
  std::string to_path{};
  std::string text{};
  MailKind kind = MailKind::kPeer;
  // The recipient's role when its header is not saved yet, from a caller
  // that started it (a child is not saved until its first step is over).
  json to_delegation = json::object();
  std::string id{};  // set to send the same message again as itself
  int hops = 0;
};

// Sends it if the rule allows; returns why not, or empty. Waking a recipient
// that has no runtime is the caller's, which knows how to start one.
std::string SendSessionMail(SessionMail mail);

// The session file of a session this one may message by choice (its society,
// or a live yolo peer in its folder), by id or file stem, or empty.
std::string LinkedSessionPath(const std::string& id);

// Those sessions plus the workspace's others: [{id,title,linked}]. Self
// excluded.
std::vector<json> SessionSummaries();

}  // namespace uagent
#endif
