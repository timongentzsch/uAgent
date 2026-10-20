// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_BOOTSTRAP_INTERNAL_H_
#define UAGENT_SRC_APP_BOOTSTRAP_INTERNAL_H_
// Module-private split of the bootstrap: bootstrap.cc builds a session,
// bootstrap_tools.cc assembles its tools and approvals.cc decides what it
// may run.

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
                                           bool tool_copy);
std::vector<Tool> BuildTools(AppContext& context,
                             const std::filesystem::path& workspace,
                             const json& trusted_snapshot,
                             std::vector<Skill> skills, std::string& error);
Agent::Approver MakeApprover(AppContext* app);
Agent::ToolRefresher MakeToolRefresher(AppContext* app);

}  // namespace uagent

#endif  // UAGENT_SRC_APP_BOOTSTRAP_INTERNAL_H_
