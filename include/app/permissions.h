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

struct RuntimeConfig;

std::string PermissionKey(const Tool& tool, const json& arguments,
                          ApprovalClass required);

// What approving this call risks, for the headline of its approval: runs,
// writes, network, outside (the folder `root`). [{id, label}], most serious
// first; empty for a call that only reads.
json ApprovalRisks(const Tool& tool, const json& arguments,
                   const std::string& root);

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
