// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
#define UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
// One model-facing search tool with independently selectable hosted backends.

#include <string>
#include <vector>

#include "include/api.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/providers.h"
#include "include/tools/tool.h"

namespace uagent {

enum class WebSearchBackend { kNone, kResponses, kOpenRouter };

struct WebSearchRoute {
  WebSearchBackend backend = WebSearchBackend::kNone;
  std::string base_url;
  std::string api_key;
  std::string model;
  // From a `:effort` suffix on UAGENT_WEB_SEARCH_MODEL; empty means the
  // UAGENT_WEB_SEARCH_EFFORT default.
  std::string effort;

  bool Valid() const {
    return backend != WebSearchBackend::kNone && !base_url.empty() &&
           !api_key.empty() && !model.empty();
  }
};

struct WebSearchResult {
  std::string text;
  json annotations = json::array();
  int64_t searches = 0;
  bool truncated = false;
};

WebSearchRoute SelectWebSearchRoute(
    const Api& api, const std::vector<NamedProvider>& providers);
json WebSearchRequest(const WebSearchRoute& route, const RuntimeConfig& config,
                      const std::string& prompt);
WebSearchResult ParseResponsesSearch(const json& response);
Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                   std::vector<NamedProvider> providers);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
