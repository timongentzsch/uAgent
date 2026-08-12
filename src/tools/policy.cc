// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {
namespace {

uint32_t CapabilityNamed(const std::string& name) {
  if (name == "inspect") return Capability(ToolCapability::kInspect);
  if (name == "execute") return Capability(ToolCapability::kExecute);
  if (name == "mutate") return Capability(ToolCapability::kMutate);
  if (name == "delegate") return Capability(ToolCapability::kDelegate);
  if (name == "external") return Capability(ToolCapability::kExternal);
  return 0;
}

bool ExactRunAllowed(const ToolPolicy& policy, const json& arguments) {
  std::string command = JsonValue(arguments, "command", "");
  return std::find(policy.run_allowlist.begin(), policy.run_allowlist.end(),
                   command) != policy.run_allowlist.end();
}

void ReadStringArray(const char* name, std::vector<std::string>& values,
                     std::string& error) {
  std::string configured = EnvStr(name);
  if (configured.empty()) return;
  json parsed = json::parse(configured, nullptr, false);
  if (!parsed.is_array() ||
      !std::all_of(parsed.begin(), parsed.end(),
                   [](const json& value) { return value.is_string(); })) {
    error = std::string(name) + " must be a JSON string array";
    return;
  }
  for (const json& value : parsed) {
    values.push_back(value.get<std::string>());
  }
}

}  // namespace

ToolPolicy ToolPolicyFromEnvironment() {
  ToolPolicy policy;
  std::string configured = Trim(EnvStr("UAGENT_TOOL_CAPABILITIES"));
  if (!configured.empty()) {
    policy.allowed = 0;
    for (const std::string& entry : SplitPathList(configured, ',')) {
      std::string name = Trim(entry);
      uint32_t capability = CapabilityNamed(name);
      if (!capability) {
        policy.error = "unknown tool capability: " + name;
        policy.allowed = 0;
        break;
      }
      policy.allowed |= capability;
    }
  }

  ReadStringArray("UAGENT_TOOL_ALLOWLIST", policy.tool_allowlist, policy.error);
  ReadStringArray("UAGENT_TOOL_RUN_ALLOWLIST", policy.run_allowlist,
                  policy.error);
  return policy;
}

void ApplyToolPolicy(std::vector<Tool>& tools, const ToolPolicy& policy) {
  if (!policy.error.empty()) {
    tools.clear();
    return;
  }
  std::erase_if(tools, [&](Tool& tool) {
    if (!policy.tool_allowlist.empty() &&
        std::find(policy.tool_allowlist.begin(), policy.tool_allowlist.end(),
                  tool.name) == policy.tool_allowlist.end()) {
      return true;
    }
    bool allowed = (tool.capabilities & ~policy.allowed) == 0;
    bool allowlisted_run = tool.name == "run" && !policy.run_allowlist.empty();
    if (!allowed && !allowlisted_run) return true;
    if (tool.name != "run" || policy.run_allowlist.empty()) return false;

    tool.validate = [policy](const json& args) {
      if (!ExactRunAllowed(policy, args)) {
        return std::string("error: command is not allowed by tool policy");
      }
      if (JsonValue(args, "detach", false) ||
          JsonValue(args, "shell", "bash") != "bash") {
        return std::string(
            "error: evaluator-authorized commands use foreground bash");
      }
      // The exact allowlist is stronger authority than the general shell
      // heuristic (which normally redirects Python to run_python).
      return std::string();
    };
    tool.description +=
        " Only an evaluator-authorized exact command is allowed.";
    return false;
  });
}

}  // namespace uagent
