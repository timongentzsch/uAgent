// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "include/agent.h"
#include "include/agent/child_agent.h"
#include "include/agent/protocol.h"
#include "include/agent/session_links.h"
#include "include/agent/session_store.h"
#include "include/core/debug.h"
#include "include/core/mailbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"

namespace uagent {
namespace {
constexpr size_t kDeliveredIds = 256;

std::string Stem(const std::string& path) {
  return std::filesystem::path(path).stem().string();
}

std::string Folder(const json& header) {
  return JsonValue(JsonValue(header, "thread", json::object()), "folder", "");
}

std::string ParentSession(const json& delegation) {
  return MailboxIdFor(JsonValue(delegation, "parent_session", ""));
}

// Who may talk to whom: linked sessions, a delegated child and its parent,
// children of one parent, and a folder's coordinator and its threads. The
// sender's own session header says which it is.
bool MailAllowed(const Mail& mail, const std::string& own_path,
                 const json& role) {
  // The parent, by its session or, without one, by its owner id.
  const json& delegation = OwnDelegation();
  const std::string parent = ParentSession(delegation);
  if (!mail.from.empty() &&
      (mail.from == parent ||
       mail.from == JsonValue(delegation, "parent", ""))) {
    return true;
  }
  if (mail.sender_path.empty() || MailboxIdFor(mail.sender_path) != mail.from) {
    return false;
  }
  const std::string own = MailboxIdFor(own_path);
  const json sender = SessionHeader(mail.sender_path);
  const std::string sender_parent =
      ParentSession(JsonValue(sender, "delegation", json::object()));
  if (!sender_parent.empty() &&
      (sender_parent == own || sender_parent == parent)) {
    return true;
  }
  const std::string folder = Folder(role);
  if (!folder.empty() && MailboxIdFor(CoordinatorPath(folder)) == mail.from) {
    return true;
  }
  // A thread asks during its first turn, before its header is saved; until
  // then its mail names its folder.
  const std::string sender_folder =
      sender.empty() ? JsonValue(mail.body, "folder", "") : Folder(sender);
  if (!sender_folder.empty() &&
      MailboxIdFor(CoordinatorPath(sender_folder)) == own) {
    return true;
  }
  return SharesLink(Stem(mail.sender_path), Stem(own_path));
}
}  // namespace

bool Agent::DeliverMail(bool hold) {
  const std::string own_path = OwnSessionFile();
  const std::string own = MailboxIdFor(own_path);
  if (own.empty()) return false;
  std::vector<Mail> taken = TakeMail(own, [&](const Mail& mail) {
    if (hold && mail.delivery == MailDelivery::kWake) return false;
    return MailAllowed(mail, own_path, session_role_);
  });
  for (const Mail& mail : taken) {
    unacked_mail_.push_back(mail.id);
    if (std::find(delivered_mail_.begin(), delivered_mail_.end(), mail.id) !=
        delivered_mail_.end()) {
      continue;
    }
    delivered_mail_.push_back(mail.id);
    if (delivered_mail_.size() > kDeliveredIds) {
      delivered_mail_.erase(delivered_mail_.begin());
    }
    // The sender labels its text; it is never the user's.
    const std::string text = JsonValue(mail.body, "text", "");
    if (text.empty()) continue;
    switch (mail.delivery) {
      case MailDelivery::kPassive:
        conversation_.Push(HarnessMessage(text), MessageKind::kInternal);
        break;
      case MailDelivery::kInterrupt:
        SteeringState().Queue(text, "", true);
        RequestAbort();
        break;
      case MailDelivery::kWake:
      case MailDelivery::kStep:
        SteeringState().Queue(text, "", mail.delivery == MailDelivery::kWake);
        break;
    }
    DebugLog("mail_delivered", {{"id", mail.id},
                                {"type", mail.type},
                                {"from", mail.from},
                                {"delivery", MailDeliveryName(mail.delivery)},
                                {"latency_ms", NowMillis() - mail.created_ms}});
  }
  return !taken.empty();
}

}  // namespace uagent
