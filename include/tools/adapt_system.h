// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_ADAPT_SYSTEM_H_
#define UAGENT_INCLUDE_TOOLS_ADAPT_SYSTEM_H_

#include <functional>

#include "include/agent/adaptive_system.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// The agent's own, conversation-scoped self-directive (Agent::SelfDirective).
// Persistent instructions are files a person owns, changed through uagent.
using SelfDirectiveControl = std::function<json(const json&)>;
Tool AdaptSystemTool(const AdaptiveSystemState& state,
                     SelfDirectiveControl control);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_ADAPT_SYSTEM_H_
