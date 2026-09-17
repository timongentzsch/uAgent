// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_DELEGATION_H_
#define UAGENT_INCLUDE_AGENT_DELEGATION_H_
// Delegation context shared by the turn loop and the subagent tool. The model
// a child runs by default and the one-line runtime context describing the
// parent route live here so the request path does not reach into the tool
// surface to build them. Bodies live in src/agent/delegation.cc.

#include <string>

#include "include/api.h"

namespace uagent {

std::string DefaultSubagentModel(const Api& api);
std::string DelegationRuntimeContext(const Api& api);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_DELEGATION_H_
