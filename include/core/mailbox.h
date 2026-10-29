// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_MAILBOX_H_
#define UAGENT_INCLUDE_CORE_MAILBOX_H_
// One durable mailbox per conversation (an agent's included),
// addressed by its session id: ~/.uagent/mail/<id>/{new,cur}. A send commits
// by an atomic rename into new/. The recipient takes a message by moving it to
// cur/ and acknowledges it once its transcript is saved, so a crash in between
// delivers it again. Recipients watch new/, so delivery wakes them at once.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "include/core/file_watch.h"
#include "include/core/json.h"

namespace uagent {

// Mail has no kinds: whatever its text says, a delivered message wakes an idle
// recipient and is read at its next step by a working one, and `hold` holds
// it with the rest. Who may send what to whom is session_links.h's rule.
struct Mail {
  // Mailbox ids (MailboxIdFor), and the sender's session file, which the
  // recipient reads to decide whether the two may talk.
  std::string id, from, to, sender_path;
  int hops = 0;
  int64_t created_ms = 0, expires_ms = 0;
  json body = json::object();
};

inline constexpr size_t kMailboxPending = 64;
inline constexpr size_t kMailSenderPerMinute = 20;
inline constexpr int kMailMaxHops = 8;
inline constexpr int64_t kMailLifetimeMs = int64_t{24} * 60 * 60 * 1000;
inline constexpr size_t kMailBytes = size_t{64} * 1024;

// A session's mailbox id: its canonical session file, hashed, so every process
// that names the session by any spelling of its path reaches one mailbox.
std::string MailboxIdFor(const std::string& session_path);
// Mail from the session at `sender_path` to the mailbox `to`.
Mail MailFrom(const std::string& sender_path, std::string to, json body);
std::string MailboxDir(const std::string& id);

// Commits `mail`, filling its id and times when empty. Refuses a full inbox,
// a sender over its rate and a message forwarded kMailMaxHops times, each with
// a reason the sender can act on. A duplicate of a pending message is
// dropped. `rate_limited` is false for what a session owes another, such as a
// an agent's reports to its parent, however many.
std::string SendMail(Mail mail, bool rate_limited = true);

// Pending messages in the order they were sent. Those `accept` takes move to
// cur/ until AckMail; the rest stay pending. Expired and unreadable ones are
// set aside, never delivered.
std::vector<Mail> TakeMail(const std::string& id,
                           const std::function<bool(const Mail&)>& accept);
std::vector<Mail> PendingMail(const std::string& id);
// Whether a message is taken and not yet acknowledged.
bool MailTaken(const std::string& id);
void AckMail(const std::string& id, const std::vector<std::string>& ids);
// Taken but unacknowledged messages become pending again: a runtime starting
// after a crash delivers what its predecessor never saved.
void RecoverMail(const std::string& id);

json MailToJson(const Mail& mail);
bool MailFromJson(const json& value, Mail& mail);

// Readable when mail may have arrived; Get() is -1 where the platform cannot
// watch, and callers look again on a timer instead.
class MailboxWatch {
 public:
  explicit MailboxWatch(const std::string& id);
  int Get() const { return watch_.Get(); }
  void Drain() const { watch_.Drain(); }

 private:
  NativeWatch watch_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_MAILBOX_H_
