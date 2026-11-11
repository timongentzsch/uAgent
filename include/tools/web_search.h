// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
#define UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
// The search tool: a side request on a route that can search, selected
// independently of the conversation's.

#include <optional>
#include <string>
#include <vector>

#include "include/api/client.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"

namespace uagent {

// What to set when search is offered somewhere but cannot run on this route.
inline constexpr char kWebSearchNoModel[] =
    "web_search has no model to run on: set webSearch.model "
    "(UAGENT_WEB_SEARCH_MODEL)";

// Whether the conversation's endpoint or a configured one declares search.
bool OffersWebSearch(const Api& api, const std::vector<Endpoint>& providers);
// The route a search runs on; nothing when search is off or cannot run.
std::optional<Route> SelectWebSearchRoute(
    const Api& api, const std::vector<Endpoint>& providers);
Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                   std::vector<Endpoint> providers);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
