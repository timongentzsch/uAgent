// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_CHAT_H_
#define UAGENT_INCLUDE_APP_CHAT_H_
#include <cstdint>
#include <functional>
#include <map>
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

// Whether `text` is for members of the folder's chat and not for its
// coordinator: it names one with @ and not the coordinator, which then reads
// it without taking a turn.
bool ForMembersOnly(const std::string& folder, const std::string& text);

// A member's message as the coordinator's conversation holds it, as
// "Name: what it wrote"; empty for any other message.
std::string ChatPost(const std::string& message);

// The mailboxes of the members woken and not yet heard from.
std::set<std::string> ChatTyping(const std::string& folder);

// The chat as its coordinator is told it each turn: who is in it and who is
// typing now. Empty without members.
std::string ChatContext(const std::string& folder);

// A coordinator's chat with its members: a room where everyone hears every
// message and decides for itself whether to answer, to pass, or to wait for
// someone who is typing. The coordinator is one of the participants.
//
// What is settled here, never by a model, is who is woken. A message that
// names someone with @ wakes those it names and is read by the rest without
// a turn; one that names nobody wakes everyone. Whoever waited is woken by
// the next message. Two limits keep an exchange from running on by itself:
// UAGENT_COORDINATOR_CHAT_TURNS turns per participant for one message of the
// user's, and two messages each. The round is kept beside the coordinator's
// session, so a runtime that starts again takes it up.
class Chat {
 public:
  // `wake` starts a turn of the coordinator's own on a note.
  Chat(std::string folder, std::function<void(const std::string&)> wake);

  // The user wrote here: a new round.
  void Said(const std::string& text);
  // The coordinator's turn ended with `text`; empty when it ended without an
  // answer.
  void Answered(const std::string& text);
  // A member's turn ended. `mail` carries its answer and leaves as what the
  // coordinator reads: nothing for a pass or a wait, and quiet unless the
  // message wakes the coordinator too.
  void Heard(Mail& mail);

 private:
  enum class Answer { kMessage, kPass, kWait };
  struct Seat {
    int posts = 0;        // messages this round
    bool waited = false;  // its last answer was to wait
  };

  // Records that `who` finished a turn with `text`, and what that was.
  Answer Finished(const std::string& who, const std::string& text);
  // Mails `text` to every member but its author (`from`) and settles whom it
  // wakes. `source` is the mail it forwards, whose id makes sending it again
  // the same mail. True when it wakes the coordinator.
  bool Deliver(const std::string& from, const std::string& author,
               const std::string& text, const std::string& source = "");
  // Whether `who` may take another turn on a message, which it is `named`
  // in or which names nobody.
  bool Wakes(const std::string& who, bool any_named, bool named);
  // With nobody typing, whoever waited longest is told so.
  void Release(const std::vector<SessionInfo>& members);
  void Save() const;

  std::string folder_;
  std::function<void(const std::string&)> wake_;
  int64_t turns_ = 0;  // turns this round may still start
  // Participants woken and not yet heard from, those who chose to wait (the
  // earliest first), and what each has done this round. A member is known by
  // its mailbox, the coordinator by kSelf.
  std::set<std::string> typing_;
  std::vector<std::string> waiting_;
  std::map<std::string, Seat> seats_;
};
}  // namespace uagent
#endif  // UAGENT_INCLUDE_APP_CHAT_H_
