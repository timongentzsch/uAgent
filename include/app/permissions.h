// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_PERMISSIONS_H_
#define UAGENT_INCLUDE_APP_PERMISSIONS_H_

#include <cstdint>
#include <string>

#include "include/api.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/tools/tool.h"

namespace uagent {

enum class PermissionOverride { kDefault, kAsk, kAuto, kYolo };

const char* PermissionOverrideName(PermissionOverride mode);
bool ParsePermissionOverride(const std::string& value,
                             PermissionOverride& mode);
// An explicit override wins; kDefault defers to the configured mode.
ApprovalMode ResolveApprovalMode(PermissionOverride override,
                                 ApprovalMode configured);

std::string PermissionKey(const Tool& tool, const json& arguments,
                          ApprovalClass required);
const char* PermissionScope();

bool RepositoryPermissionAllows(const std::string& root,
                                const std::string& key);
bool RememberRepositoryPermission(const std::string& root,
                                  const std::string& key,
                                  const std::string& tool,
                                  const std::string& preview,
                                  std::string& error);
json PermissionRulesControl(const json& request);

enum class AutoPermissionDecision { kAllow, kAsk, kDeny };

struct AutoPermissionReview {
  AutoPermissionDecision decision = AutoPermissionDecision::kAsk;
  json probabilities = json::object();
  std::string error;
};

AutoPermissionReview ReviewPermission(Api& api, const RuntimeConfig& config,
                                      UsageAccumulator& usage, int64_t turn,
                                      const Tool& tool,
                                      const std::string& preview,
                                      const std::string& user_request);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_PERMISSIONS_H_
