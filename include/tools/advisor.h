// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_ADVISOR_H_
#define UAGENT_INCLUDE_TOOLS_ADVISOR_H_
// A second opinion from a different model. The advisor is a subagent with no
// tools and no memory: it reasons only about the question and the evidence the
// caller hands it, so its answer is independent of this session's state.

#include <string>
#include <vector>

#include "include/api.h"
#include "include/providers.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

// The configured advisor selection, or empty when the tool is not enabled.
std::string AdvisorModel();

Tool AdvisorTool(const Api& api, ProcessSupervisor& processes,
                 const std::vector<ModelRoute>& routes,
                 const std::vector<NamedProvider>& providers, bool debug);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_ADVISOR_H_
