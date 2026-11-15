// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_MAILBOX_H_
#define UAGENT_INCLUDE_CORE_MAILBOX_H_
// One durable mailbox per conversation: ~/.uagent/mail/<id>/{new,cur}. A send
// renames into new/; a taken message sits in cur/ until it is acknowledged.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "include/core/file_watch.h"
#include "include/core/json.h"

namespace uagent {

// Mail has no kinds: a delivered message wakes an idle recipient, and a working
// one reads it at its next step.
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

// The session's canonical file, hashed: every spelling reaches one mailbox.
std::string MailboxIdFor(const std::string& session_path);
Mail MailFrom(const std::string& sender_path, std::string to, json body);
std::string MailboxDir(const std::string& id);

// Commits `mail`, filling its id and times when empty; returns why it was
// refused, or empty. A duplicate of a pending message is dropped.
std::string SendMail(Mail mail, bool rate_limited = true);

// Pending messages in send order. Those `accept` takes move to cur/ until
// AckMail; expired and unreadable ones are set aside, never delivered.
std::vector<Mail> TakeMail(const std::string& id,
                           const std::function<bool(const Mail&)>& accept);
std::vector<Mail> PendingMail(const std::string& id);
// Whether a message is taken and not yet acknowledged.
bool MailTaken(const std::string& id);
void AckMail(const std::string& id, const std::vector<std::string>& ids);
// Taken but unacknowledged messages become pending again, after a crash.
void RecoverMail(const std::string& id);

json MailToJson(const Mail& mail);
bool MailFromJson(const json& value, Mail& mail);

// Readable when mail may have arrived; Get() is -1 where nothing can watch.
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
