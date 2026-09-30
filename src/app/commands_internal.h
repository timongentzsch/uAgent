// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_COMMANDS_INTERNAL_H_
#define UAGENT_SRC_APP_COMMANDS_INTERNAL_H_
// Slash-command handlers shared by the command translation units.
// RunSlashCommand (commands.cc) dispatches; model, session and control
// units define. Public entry points stay in include/app/commands.h.

#include <optional>
#include <string>

#include "include/app/commands.h"
#include "include/app/self_description.h"
#include "include/providers.h"

namespace uagent {

void ActivateCurrentRoute(AppSession& session);
std::string SaveSelectedModel(AppSession& session, const std::string& selected);
void HandleModels(AppSession& session, const std::string& argument,
                  CommandReply& reply);
void HandleModel(AppSession& session, const std::string& argument,
                 CommandReply& reply);

void HandleEffort(AppSession& session, const std::string& argument,
                  CommandReply& reply);
void HandleVariant(AppSession& session, const std::string& argument,
                   CommandReply& reply);
void HandleAttach(AppSession& session, const std::string& argument,
                  CommandReply& reply);
void HandleCost(const AppSession& session, CommandReply& reply);
void HandleContext(AppSession& session, CommandReply& reply);
void HandleStatus(const AppSession& session, CommandReply& reply);
void HandleInstructions(AppSession& session, const std::string& argument,
                        CommandReply& reply);
void HandleDebugConfig(const AppSession& session, const std::string& argument,
                       CommandReply& reply);
void HandleConfig(AppSession& session, const std::string& argument,
                  CommandReply& reply);
void HandleMcp(AppSession& session, const std::string& argument,
               CommandReply& reply);
void HandlePermissionRules(const std::string& argument, CommandReply& reply);
void HandleTools(AppSession& session, const std::string& argument,
                 CommandReply& reply);
json AgentsJson(const AppSession& session);
SelfDescriptionInputs DescriptionInputs(const AppSession& session);

}  // namespace uagent

#endif  // UAGENT_SRC_APP_COMMANDS_INTERNAL_H_
