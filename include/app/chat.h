// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_CHAT_H_
#define UAGENT_INCLUDE_APP_CHAT_H_
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/core/mailbox.h"

namespace uagent {
// The members of a folder's chat: the threads its coordinator added under a
// name and a persona, oldest first.
std::vector<SessionInfo> ChatMembers(const std::string& folder);

// Whether `text` is for members of the folder's chat alone: it names one with
// @ and not the coordinator.
bool ForMembersOnly(const std::string& folder, const std::string& text);

// A member's message as the coordinator's conversation holds it, as
// "Name: what it wrote"; empty for any other message.
std::string ChatPost(const std::string& message);

// A coordinator's chat with its members. What anyone writes reaches every
// member; who is woken to answer it is settled here, never by a model: those
// a message names with @, or everyone when it names nobody. A member answers
// or passes, and the coordinator takes the floor once nobody is owed a turn,
// unless the user's message was for members alone.
// One message of the user's starts at most UAGENT_COORDINATOR_CHAT_TURNS
// member turns, so no exchange runs on by itself. The round is kept beside
// the coordinator's session, so a runtime that starts again takes it up.
class Chat {
 public:
  explicit Chat(std::string folder);

  // What the user (`person`) or the coordinator wrote here.
  void Said(const std::string& text, bool person);
  // A member's turn ended. `mail` carries its words and leaves as what the
  // coordinator reads: nothing for a pass, and quiet until the floor is its.
  void Heard(Mail& mail);

 private:
  // Mails `text` to every member but its author (`from`), waking those it
  // names, or all of them when it names nobody and is `open`. `source` is
  // the message it forwards, whose id makes sending it again the same mail.
  void Tell(const std::vector<SessionInfo>& members, const std::string& from,
            const std::string& author, const std::string& text, bool open,
            const std::string& source = "");
  void Save() const;

  std::string folder_;
  int64_t turns_ = 0;  // member turns this round may still start
  // A member wrote since the coordinator last had the floor, and whether the
  // floor returns to it when the members are done.
  bool posted_ = false, moderated_ = true;
  // Mailboxes of the members owing an answer. A set: a member woken twice
  // answers once when the second message reaches it mid-turn, so the floor
  // may return a message early, never late.
  std::set<std::string> awaited_;
};
}  // namespace uagent
#endif  // UAGENT_INCLUDE_APP_CHAT_H_
