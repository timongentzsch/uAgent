// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_THREAD_LINK_H_
#define UAGENT_INCLUDE_APP_THREAD_LINK_H_
#include <optional>
#include <string>

#include "include/core/json.h"
#include "include/core/mailbox.h"

namespace uagent::session {
// A thread's link to the coordinator that started it, from the thread's
// session header: where its approvals, questions and reports go, and how
// often in a row the coordinator has messaged it.
class ThreadLink {
 public:
  ThreadLink(json thread, std::string path, std::string id);

  // Puts a decision to the coordinator from the brief it wrote and the raw
  // request, never the thread's own reasoning. `kind` is "approval request"
  // or "question", `asks` what the thread wants. If no coordinator takes it,
  // the thread escalates `interaction` to the user itself.
  void Ask(const std::string& interaction, const std::string& kind,
           const std::string& asks, const std::string& data,
           const std::string& title) const;

  // A finished turn is an event for the coordinator, with the turn's answer
  // so reading it costs no round.
  void Report(const std::string& reason, const std::string& title,
              const std::string& answer);
  // Sends again a report the coordinator's full inbox refused.
  void Resend();
  // Whether a report still waits to be sent: its runtime stays for it.
  bool Owes() const { return unsent_.has_value(); }

  // Why a message may not enter the thread, or empty. A coordinator may send
  // kStreak in a row with nobody else speaking here, so the two can never
  // loop on each other's reports.
  std::string Admit(bool from_coordinator);

 private:
  static constexpr int kStreak = 3;
  json thread_;
  std::string path_, id_;
  int streak_ = 0;  // coordinator messages since anyone else spoke
  std::optional<Mail> unsent_;
};
}  // namespace uagent::session
#endif
