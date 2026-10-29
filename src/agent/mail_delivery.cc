// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "include/agent/agent.h"
#include "include/agent/child_agent.h"
#include "include/agent/protocol.h"
#include "include/agent/session_links.h"
#include "include/agent/session_store.h"
#include "include/core/debug.h"
#include "include/core/mailbox.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
constexpr size_t kDeliveredIds = 256;

// Who may talk to whom is MayMail's; this process is the recipient, and
// knows its role before its header is saved.
bool MailAllowed(const Mail& mail, const std::string& own_path,
                 const json& role) {
  return !mail.sender_path.empty() &&
         MailboxIdFor(mail.sender_path) == mail.from &&
         MayMail(SessionParty(mail.sender_path),
                 SessionParty(own_path, AgentOf(role)));
}
}  // namespace

bool Agent::DeliverMail() {
  const std::string own_path = OwnSessionFile();
  const std::string own = MailboxIdFor(own_path);
  if (own.empty()) return false;
  std::vector<Mail> taken = TakeMail(own, [&](const Mail& mail) {
    return MailAllowed(mail, own_path, session_role_);
  });
  for (Mail& mail : taken) {
    const bool seen = std::find(delivered_mail_.begin(), delivered_mail_.end(),
                                mail.id) != delivered_mail_.end();
    // The sender labels its text; it is never the user's. Trimmed as input
    // is, so the message it becomes is known for this mail's.
    const std::string text = Trim(JsonValue(mail.body, "text", ""));
    // Mail that changes nothing has no save to be acknowledged by: one
    // without text, or one a save already holds.
    if ((text.empty() || seen) &&
        std::ranges::find(unacked_mail_, mail.id) == unacked_mail_.end()) {
      AckMail(own, {mail.id});
      continue;
    }
    unacked_mail_.push_back(mail.id);
    if (seen) continue;
    delivered_mail_.push_back(mail.id);
    if (delivered_mail_.size() > kDeliveredIds) {
      delivered_mail_.erase(delivered_mail_.begin());
    }
    // Mail wakes its recipient: an idle one starts a turn on it, and what
    // that turn sends is one hop further along.
    Arrive(text, mail.id, mail.hops + 1);
    DebugLog("mail_delivered", {{"id", mail.id},
                                {"from", mail.from},
                                {"latency_ms", NowMillis() - mail.created_ms}});
  }
  return !taken.empty();
}

}  // namespace uagent
