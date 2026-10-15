// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
#define UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
// One model-facing search tool over OpenRouter's hosted `openrouter:web_search`
// server tool. The route is selected independently of the conversation's.

#include <optional>
#include <string>
#include <vector>

#include "include/api/client.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"

namespace uagent {

struct WebSearchResult {
  std::string text;
  json annotations = json::array();
  int64_t searches = 0;
  bool truncated = false;
};

// Whether the conversation's endpoint or a configured one declares search; the
// tool is offered then, and a call says what is missing.
bool OffersWebSearch(const Api& api, const std::vector<Endpoint>& providers);
// The route a search runs on, with the effort `webSearch.model` names; nothing
// when search is off or no endpoint can run it.
std::optional<Route> SelectWebSearchRoute(
    const Api& api, const std::vector<Endpoint>& providers);
json WebSearchRequest(const Route& route, const std::string& prompt);
WebSearchResult ParseWebSearch(const json& response);
Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                   std::vector<Endpoint> providers);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
