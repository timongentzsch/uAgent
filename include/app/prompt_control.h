// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_PROMPT_CONTROL_H_
#define UAGENT_INCLUDE_APP_PROMPT_CONTROL_H_
#include <functional>
#include <string>

#include "include/agent/adaptive_system.h"
#include "include/core/json.h"

namespace uagent {
// All interfaces edit the same documents. A null state excludes conversation
// scope; base and context come from the active agent's actual request builder.
json PromptControl(const json& request, AdaptiveSystemState* state,
                   const std::string& base, const json& context);
using PromptController = std::function<json(const json&)>;
json PromptCommand(const std::string& argument, const PromptController& control,
                   bool browser = false);
}  // namespace uagent
#endif
