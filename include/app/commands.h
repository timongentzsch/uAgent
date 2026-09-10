// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_COMMANDS_H_
#define UAGENT_INCLUDE_APP_COMMANDS_H_
// Slash commands and the session session they read or change.

#include <cstdint>
#include <string>
#include <vector>

#include "include/agent.h"
#include "include/api.h"
#include "include/app/bootstrap.h"
#include "include/cli.h"
#include "include/media/attachments.h"
#include "include/ui/display.h"

namespace uagent {

// The parts of a run a command may touch. Everything else reachable from the
// application is deliberately out of reach, so the coupling stays visible.
struct AppSession {
  AppContext& context;
  std::vector<Attachment>& attachments;
  std::string& session_file;
  uint64_t& saved_revision;

  // Named for what they reach, not for the type they return: a member may not
  // shadow the type name it hands back.
  AppRuntime& Runtime() const { return context.runtime; }
  Api& ApiClient() const { return context.runtime.api; }
  Agent& ActiveAgent() const { return *context.agent; }
};

// Returns true when the command ends the session and fills the same semantic
// result a non-terminal client receives in command.completed.
bool RunSlashCommand(AppSession& session, const ParsedSlashCommand& command,
                     json& result);

// Adopts the journal of a session that was just resumed into.
void LoadSessionJournal(AppSession& session, const std::string& previous_path);

void SaveSessionSettings(AppSession& session);

StatusView SessionStatusView(const AppSession& session);
json SessionControl(AppSession& session, const json& request);
json PermissionControl(AppContext& context, const json& request);
json ActivityControl(ProcessSupervisor& processes, const json& request);
std::string ActivityText(const json& result);
json ActivityCommand(AppSession& session, const ParsedSlashCommand& command);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_COMMANDS_H_
