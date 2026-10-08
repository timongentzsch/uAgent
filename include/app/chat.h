// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_CHAT_H_
#define UAGENT_INCLUDE_APP_CHAT_H_
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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

// Those of `names` that `text` opens with, as "Sam, …" or "Sam and Lin: …"
// does, in any case. A name anywhere else addresses nobody.
std::set<std::string> Addressed(const std::string& text,
                                const std::vector<std::string>& names);

// Whether `text` is for members of the folder's chat and not for its
// coordinator: it opens with their names and not the coordinator's, which
// then reads it without taking a turn.
bool ForMembersOnly(const std::string& folder, const std::string& text);

// The mailboxes of the members woken and not yet heard from.
std::set<std::string> ChatTyping(const std::string& folder);

// A coordinator's chat with its members: a room where everyone hears every
// message and decides for itself whether to answer, to pass, or to wait for
// someone who is typing. The coordinator is one of the participants.
//
// What is settled here, never by a model, is who is woken. A message that
// opens with names wakes those it names and is read by the rest without a
// turn; their answer goes back to who asked. Any other message wakes
// everyone. Whoever waited is woken by the next message. Two limits keep an
// exchange from running on by itself:
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
  // The chat as its coordinator is told it each turn: who is in it and who
  // is typing now. Empty without members.
  std::string Context() const;

 private:
  enum class Answer { kMessage, kPass, kWait };
  struct Seat {
    int64_t turns = 0;    // turns begun this round
    int posts = 0;        // messages this round
    bool waited = false;  // its last answer was to wait
    // Who put the message it is answering to it by name; empty for the user.
    std::optional<std::string> asker;
  };

  // Records that `who` finished a turn with `text`, and what that was.
  // `asker` leaves as who had asked it.
  Answer Finished(const std::string& who, const std::string& text,
                  std::optional<std::string>& asker);
  // Mails `text` to every member but its author (`from`) and settles whom it
  // wakes: those it opens with, else the `asker` it answers, else everyone.
  // `source` is the mail it forwards, whose id makes sending it again the
  // same mail. `late`, it answers an earlier round and wakes nobody. True
  // when it wakes the coordinator.
  bool Deliver(const std::string& from, const std::string& author,
               const std::string& text,
               const std::optional<std::string>& asker = {},
               const std::string& source = "", bool late = false);
  // Whether `who` may take another turn on a message for `some` only, which
  // it is `one` of; `asker` is who put it to them by name.
  bool Wakes(const std::string& who, bool some, bool one,
             const std::optional<std::string>& asker);
  // With nobody typing, whoever waited longest is told so.
  void Release(const std::vector<SessionInfo>& members);
  void Save() const;

  std::string folder_;
  std::function<void(const std::string&)> wake_;
  int64_t each_ = 0;   // turns one participant may take this round
  int64_t round_ = 0;  // when the user's message that began this round came
  // Participants woken and not yet heard from, those who chose to wait (the
  // earliest first), and what each has done this round. A member is known by
  // its mailbox, the coordinator by kSelf.
  std::set<std::string> typing_;
  std::vector<std::string> waiting_;
  std::map<std::string, Seat> seats_;
  // The members' mails counted this round and how each left: a runtime that
  // died before its session recorded one is handed it again.
  enum Read { kNothing, kQuiet, kWaking };
  std::map<std::string, int> heard_;
};
}  // namespace uagent
#endif  // UAGENT_INCLUDE_APP_CHAT_H_
