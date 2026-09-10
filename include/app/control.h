// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_CONTROL_H_
#define UAGENT_INCLUDE_APP_CONTROL_H_
#include <string>

#include "include/core/json.h"
namespace uagent {
json ManagementControl(const json& request);
json ManagementCommand(const std::string& kind, const std::string& argument);
// Metadata-only helper: no provider bootstrap, agent turn, or project trust
// prompt.
int ControlMain(const std::string& argument);
json ControlProcess(const json& request);
}  // namespace uagent
#endif
