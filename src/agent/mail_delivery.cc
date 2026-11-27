// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/agent.h"
#include "include/agent/protocol.h"
#include "include/agent/session_links.h"
#include "include/agent/session_store.h"
#include "include/core/debug.h"
#include "include/core/env.h"
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
         SessionIdFor(mail.sender_path) == mail.from &&
         MayMail(SessionParty(mail.sender_path),
                 SessionParty(own_path, AgentOf(role)),
                 mail.body.contains("state"));
}
}  // namespace

bool Agent::DeliverMail(bool with_notes) {
  const std::string own_path = OwnSessionFile();
  const std::string own = SessionIdFor(own_path);
  if (own.empty()) return false;
  // A note from an agent of this one does not start a turn, nor a model call
  // of its own: it is read with the next one made anyway.
  std::vector<Mail> taken = TakeMail(own, [&](const Mail& mail) {
    return MailAllowed(mail, own_path, session_role_) &&
           (with_notes || mail.body.contains("state") ||
            SessionParty(mail.sender_path).parent != own);
  });
  for (Mail& mail : taken) {
    const bool seen = std::find(delivered_mail_.begin(), delivered_mail_.end(),
                                mail.id) != delivered_mail_.end();
    // The sender labels its text; it is never the user's. Trimmed as input
    // is, so the message it becomes is known for this mail's.
    std::string text = Trim(JsonValue(mail.body, "text", ""));
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
    json origin = {
        {"kind", "message"}, {"mail", mail.id}, {"hops", mail.hops + 1}};
    if (mail.body.contains("agent")) origin["agent"] = mail.body["agent"];
    size_t envelope = JsonValue(mail.body, "envelope", size_t{0});
    // A report carries its state; a note is other mail with its sender.
    if (mail.body.contains("state")) {
      origin["kind"] = "agent";
      if (judge_report_ &&
          JsonValue(mail.body, "state", "") != "needs decision") {
        std::string judged = judge_report_(mail.sender_path, text, turn_id_);
        // A withheld report is the harness's note, shown whole.
        if (judged != text) envelope = 0;
        text = std::move(judged);
      }
    }
    if (envelope > 0) origin["envelope"] = envelope;
    Arrive(text, std::move(origin));
    DebugLog("mail_delivered", {{"id", mail.id},
                                {"from", mail.from},
                                {"latency_ms", NowMillis() - mail.created_ms}});
  }
  return !taken.empty();
}

}  // namespace uagent
