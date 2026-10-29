// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_AGENT_LINK_H_
#define UAGENT_INCLUDE_APP_AGENT_LINK_H_
// An agent's side of the parent that started it: decisions, questions and
// results sent up as mail, used by the agent's session worker.

#include <optional>
#include <string>

#include "include/agent/session_links.h"

namespace uagent::session {
// Starts the runtime of `path` unless a process holds it already and so
// reads its mail itself: a runtime, or a headless run. False when it cannot
// be reached.
bool Reach(const std::string& path);

// An agent's link to its parent session `parent` (a file, empty when it is
// gone): where its approvals, questions and reports go.
class AgentLink {
 public:
  AgentLink(std::string path, std::string id, std::string parent);

  // Puts a decision to the parent: the raw request, never the agent's own
  // reasoning. `kind` is "approval request" or "question", `asks` what the
  // agent wants. If no parent takes it, the agent escalates `interaction` to
  // the person itself.
  void Ask(const std::string& interaction, const std::string& kind,
           const std::string& asks, const std::string& data,
           const std::string& title) const;

  // A finished turn is an event for the parent, with the turn's answer so
  // reading it costs no round.
  void Report(const std::string& reason, const std::string& title,
              const std::string& answer);
  // Sends again a report the parent's full inbox refused.
  void Resend();
  // Whether a report still waits to be sent: its runtime stays for it.
  bool Owes() const { return unsent_.has_value(); }

 private:
  // How the parent's mail names this agent: agent <id> "<title>".
  std::string Label(const std::string& title) const;

  std::string path_, id_, parent_;
  std::optional<SessionMail> unsent_;
};
}  // namespace uagent::session
#endif
