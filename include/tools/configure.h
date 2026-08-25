// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_CONFIGURE_H_
#define UAGENT_INCLUDE_TOOLS_CONFIGURE_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/tools/tool.h"

namespace uagent {

// Builds a proposal from live state. Supplied by the application so the tool
// never holds a stale snapshot of the configuration layers.
using ConfigProposalFactory = std::function<ConfigProposal(
    ConfigProposalScope, const std::vector<ConfigChange>&)>;

// Requests a persistent configuration change. The tool prepares and previews;
// the mandatory-human approval lane decides, and only an approved proposal can
// be committed. There is deliberately no commit argument the model can set.
Tool ConfigureTool(ConfigProposalFactory prepare,
                   std::shared_ptr<ConfigProposalStore> store);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_CONFIGURE_H_
