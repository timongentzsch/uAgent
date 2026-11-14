// Copyright 2026 Timon Gentzsch

#include "include/agent/child_agent.h"

#include <string>
#include <utility>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/sandbox.h"

namespace uagent {

EnvironmentOverrides ChildAgentEnvironment(Route route,
                                           const std::string& session) {
  EnvironmentOverrides environment = RouteEnvironment(route);
  environment.emplace_back("UAGENT_INTERNAL_DEPTH",
                           std::to_string(AgentDepth() + 1));
  environment.emplace_back("UAGENT_INTERNAL_API_KEY", std::move(route.api_key));
  environment.emplace_back("UAGENT_INTERNAL_USAGE_FILE", UsageLedger(session));
  // A child runs under the sandbox this process's commands run under now, not
  // one composed from a configuration that may have been loosened since.
  environment.emplace_back("UAGENT_INTERNAL_SANDBOX", SandboxInheritance());
  return environment;
}

std::string OwnSessionFile() { return EnvStr("UAGENT_INTERNAL_SESSION_PATH"); }

std::string PlanFile() {
  const std::string session = OwnSessionFile();
  return session.empty() ? session : session + kSessionPlanSuffix;
}

}  // namespace uagent
