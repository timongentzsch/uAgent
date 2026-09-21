// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_COMMANDS_INTERNAL_H_
#define UAGENT_SRC_APP_COMMANDS_INTERNAL_H_
// Slash-command handlers shared by the command translation units.
// RunSlashCommand (commands.cc) dispatches; model, session and control
// units define. Public entry points stay in include/app/commands.h.

#include <optional>
#include <string>

#include "include/app/commands.h"
#include "include/providers.h"

namespace uagent {

void ActivateCurrentRoute(AppSession& session);
void SaveSelectedModel(AppSession& session, const std::string& selected);
void HandleModels(AppSession& session, const std::string& argument);
void HandleModel(AppSession& session, const std::string& argument);
void PersistSelectionSuffix(AppSession& session);
void HandleEffort(AppSession& session, const std::string& argument);
void HandleVariant(AppSession& session, const std::string& argument);
void HandleCompact(AppSession& session);
void HandleAttach(AppSession& session, const std::string& argument);
void HandleCost(const AppSession& session);
void HandleContext(AppSession& session);
void HandleStatus(const AppSession& session);
void HandleDebugConfig(const AppSession& session, const std::string& argument);
void HandleTools(AppSession& session, const std::string& argument);
json AgentsJson(const AppSession& session);
SelfDescriptionInputs DescriptionInputs(const AppSession& session);
json CommandResult(const AppSession& session,
                   const ParsedSlashCommand& command);

}  // namespace uagent

#endif  // UAGENT_SRC_APP_COMMANDS_INTERNAL_H_
