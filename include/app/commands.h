// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_COMMANDS_H_
#define UAGENT_INCLUDE_APP_COMMANDS_H_
// Slash commands and the session state they read or change.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "include/agent/agent.h"
#include "include/api/client.h"
#include "include/app/bootstrap.h"
#include "include/cli/cli.h"
#include "include/core/style.h"
#include "include/media/attachments.h"

namespace uagent {

// The parts of a run a command may touch.
struct AppSession {
  AppContext& context;
  std::vector<Attachment>& attachments;
  std::string& session_file;
  uint64_t& saved_revision;

  // Named for what they reach: a member may not shadow the type it returns.
  AppRuntime& Runtime() const { return context.runtime; }
  Api& ApiClient() const { return context.runtime.api; }
  Agent& ActiveAgent() const { return *context.agent; }
  // Save changed state and flush pending journal events before acknowledging.
  bool Save(std::string& error, bool force = false);
};

// A command's result and its terminal text; stdout is never redirected.
struct CommandReply {
  json result;
  std::string output;
  void Print(const char* format, ...) __attribute__((format(printf, 2, 3)));
  void Note(Tone tone, std::string_view text);
};
CommandReply RunSlashCommand(AppSession& session,
                             const ParsedSlashCommand& command);

// Adopts the journal of a session that was just resumed into.
void LoadSessionJournal(AppSession& session, const std::string& previous_path);

void SaveSessionSettings(AppSession& session);
// Takes a saved conversation's own settings into `manager`. A choice the
// manager already holds (a flag of this run) stays. Returns those taken.
RuntimeConfig::Values RestoreChoices(const json& settings,
                                     ConfigManager& manager);

json SessionControl(AppSession& session, const json& request);
json PermissionControl(AppContext& context, const json& request);
// What the approval control shows: the conversation's own mode or "default",
// the mode in effect and the inherited one (Auto for an agent).
json ApprovalState(const EffectiveConfigSnapshot& configured, bool agent);
json ActivityControl(ActivitySupervisor& processes, const json& request);
std::string ActivityText(const json& result);
json ActivityCommand(AppSession& session, const ParsedSlashCommand& command);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_COMMANDS_H_
