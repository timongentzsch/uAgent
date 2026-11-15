// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_BOOTSTRAP_INTERNAL_H_
#define UAGENT_SRC_APP_BOOTSTRAP_INTERNAL_H_
// Module-private parts of the bootstrap, shared by bootstrap.cc,
// bootstrap_tools.cc and approvals.cc.

#include <filesystem>
#include <string>
#include <vector>

#include "include/agent/agent.h"
#include "include/app/asset_store.h"
#include "include/app/bootstrap.h"
#include "include/cli/cli.h"
#include "include/core/json.h"
#include "include/core/skills.h"
#include "include/tools/tool.h"

namespace uagent {

bool Confirm(InteractionRequest request);
bool InteractiveApprovalAvailable();
session::AssetStoreResult StoreSessionFile(const std::string& session_path,
                                           const std::string& path,
                                           const std::string& name,
                                           bool tool_copy, bool shared = false);
std::vector<Tool> BuildTools(AppContext& context,
                             const std::filesystem::path& workspace,
                             const json& trusted_snapshot,
                             std::vector<Skill> skills, std::string& error);
Agent::Approver MakeApprover(AppContext* app);
Agent::ToolRefresher MakeToolRefresher(AppContext* app);

}  // namespace uagent

#endif  // UAGENT_SRC_APP_BOOTSTRAP_INTERNAL_H_
