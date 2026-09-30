// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_MAILBOX_H_
#define UAGENT_INCLUDE_CORE_MAILBOX_H_
// One durable mailbox per agent (a session, a coordinator, a delegated child),
// addressed by its session id: ~/.uagent/mail/<id>/{new,cur}. A send commits
// by an atomic rename into new/. The recipient takes a message by moving it to
// cur/ and acknowledges it once its transcript is saved, so a crash in between
// delivers it again. Recipients watch new/, so delivery wakes them at once.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent {

// How a message enters its recipient: a wake starts a turn when it is idle, a
// step waits for its next turn, a passive one is only recorded, and an
// interrupt stops the running turn first.
enum class MailDelivery { kWake, kStep, kPassive, kInterrupt };

// Message types. The type, not the text, says what a message is.
inline constexpr const char* kMailTaskCompleted = "task.completed";
inline constexpr const char* kMailTaskFailed = "task.failed";
inline constexpr const char* kMailTaskProgress = "task.progress";
inline constexpr const char* kMailAsk = "ask";
inline constexpr const char* kMailSteer = "steer";
inline constexpr const char* kMailCancel = "cancel";
inline constexpr const char* kMailNote = "note";

struct Mail {
  // Mailbox ids (MailboxIdFor), and the sender's session file, which the
  // recipient reads to decide whether the two may talk.
  std::string id, from, to, type, sender_path;
  // The task or conversation it belongs to, and the message that caused it.
  std::string correlation_id, causation_id, reply_to;
  MailDelivery delivery = MailDelivery::kWake;
  int hops = 0;
  int64_t created_ms = 0, expires_ms = 0;
  json body = json::object();
};

inline constexpr size_t kMailboxPending = 64;
inline constexpr size_t kMailSenderPerMinute = 20;
inline constexpr int kMailMaxHops = 8;
inline constexpr int64_t kMailLifetimeMs = 24 * 60 * 60 * 1000;
inline constexpr size_t kMailBytes = 64 * 1024;

// A session's mailbox id: its canonical session file, hashed, so every process
// that names the session by any spelling of its path reaches one mailbox.
std::string MailboxIdFor(const std::string& session_path);
std::string MailboxDir(const std::string& id);

// Commits `mail`, filling its id and times when empty. Refuses a full inbox,
// a sender over its rate and a message forwarded kMailMaxHops times, each with
// a reason the sender can act on. A duplicate of a pending message is dropped,
// and a progress report replaces the pending one for its task.
std::string SendMail(Mail mail);

// Pending messages in the order they were sent. Those `accept` takes move to
// cur/ until AckMail; the rest stay pending. Expired and unreadable ones are
// set aside, never delivered.
std::vector<Mail> TakeMail(const std::string& id,
                           const std::function<bool(const Mail&)>& accept);
std::vector<Mail> PendingMail(const std::string& id);
void AckMail(const std::string& id, const std::vector<std::string>& ids);
// Taken but unacknowledged messages become pending again: a runtime starting
// after a crash delivers what its predecessor never saved.
void RecoverMail(const std::string& id);

json MailToJson(const Mail& mail);
bool MailFromJson(const json& value, Mail& mail);
const char* MailDeliveryName(MailDelivery delivery);

// Readable when mail may have arrived; Get() is -1 where the platform cannot
// watch, and callers look again on a timer instead.
class MailboxWatch {
 public:
  explicit MailboxWatch(const std::string& id);
  int Get() const { return fd_.Get(); }
  void Drain() const;

 private:
  Fd fd_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_MAILBOX_H_
