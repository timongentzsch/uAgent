// Copyright 2026 Timon Gentzsch

#include "include/tools/web_search.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/api/citations.h"
#include "include/api/retry.h"
#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
namespace {

ToolResult SearchError(const std::string& detail) {
  return ToolFailure(ToolErrorCode::kRemoteError, "web_search " + detail);
}

}  // namespace

bool OffersWebSearch(const Api& api, const std::vector<Endpoint>& providers) {
  return api.config.web_search_backend != "off" &&
         (api.features.Has(Feature::kHostedSearch) ||
          std::ranges::any_of(providers, [](const Endpoint& provider) {
            return provider.features.Has(Feature::kHostedSearch);
          }));
}

std::optional<Route> SelectWebSearchRoute(
    const Api& api, const std::vector<Endpoint>& providers) {
  const RuntimeConfig& config = api.config;
  if (config.web_search_backend == "off") return std::nullopt;

  // UAGENT_WEB_SEARCH_MODEL follows the shared selection schema. A provider
  // scope makes it a route of its own; a bare id only renames the model on
  // whichever candidate wins, which is how it behaved before the schema.
  ModelSelection selection = ParseModelSelection(config.web_search_model);
  const auto usable = [&](Route route) -> std::optional<Route> {
    route.reasoning_effort = selection.effort;
    if (route.api_key.empty() || route.base_url.empty() ||
        route.model.empty()) {
      return std::nullopt;
    }
    return route;
  };
  if (selection.base.find('/') != std::string::npos) {
    ProviderCatalog catalog = SessionProviderCatalog();
    if (std::optional<Route> route = ResolveModelRoute(
            catalog.models, catalog.providers, selection.base)) {
      // An explicit selection is authoritative: an endpoint that declares no
      // search disables it rather than silently redirecting the request to a
      // different endpoint.
      if (!route->features.Has(Feature::kHostedSearch)) return std::nullopt;
      if (route->api_key.empty()) route->api_key = kPlaceholderApiKey;
      return usable(std::move(*route));
    }
  }
  const auto named = [&](Route route) {
    if (!selection.base.empty()) route.model = selection.base;
    return usable(std::move(route));
  };

  // The conversation's own route, then the first configured endpoint that
  // offers search. Another endpoint has no model of the conversation's to
  // borrow: it searches with the one UAGENT_WEB_SEARCH_MODEL names.
  if (api.features.Has(Feature::kHostedSearch)) {
    if (std::optional<Route> route = named(api)) return route;
  }
  for (const Endpoint& provider : providers) {
    if (!provider.features.Has(Feature::kHostedSearch)) continue;
    Route route;
    static_cast<Endpoint&>(route) = provider;
    return named(std::move(route));
  }
  return std::nullopt;
}

Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                   std::vector<Endpoint> providers) {
  Tool t = MakeTool(
      "web_search",
      "Search the web with cited sources; put up to " +
          std::to_string(WebSearchMaxUses()) +
          " related queries in one call. Include dates or cutoffs in recency "
          "queries. Do not repeat a search.",
      json::parse(R"json({"type":"object","properties":{
          "queries":{"type":"array","items":{"type":"string","minLength":1},
            "minItems":1,"maxItems":3}},
          "required":["queries"]})json"),
      [&api, &usage, providers](const json& a,
                                const ToolContext& context) -> ToolResult {
        const std::optional<Route> found = SelectWebSearchRoute(api, providers);
        if (!found) {
          return ToolFailure(ToolErrorCode::kUnavailable, kWebSearchNoModel);
        }
        const Route& active = *found;
        std::vector<std::string> queries;
        queries.reserve(a["queries"].size());
        for (const json& value : a["queries"]) {
          std::string query = Trim(value.get<std::string>());
          if (query.empty()) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "queries must not be blank");
          }
          queries.push_back(std::move(query));
        }
        if (queries.size() > static_cast<size_t>(WebSearchMaxUses())) {
          return ToolFailure(ToolErrorCode::kLimitExceeded,
                             "too many queries for the configured "
                             "web search use limit");
        }
        std::string numbered;
        for (size_t i = 0; i < queries.size(); ++i) {
          numbered += std::to_string(i + 1) + ". " + queries[i] + "\n";
        }
        std::string prompt = "Current host date: " + LocalDay() +
                             ". Search the web, then answer each numbered "
                             "query concisely with "
                             "source URLs. For latest/newest claims, compare "
                             "publication dates from the official index. Use "
                             "only source-supported claims; preserve provider/"
                             "model scope and omit unasked pricing:\n" +
                             numbered;
        // Attempts of the usual length, within the tool's budget.
        RuntimeConfig config = api.config;
        config.request_timeout_s = WebSearchTimeoutSeconds();
        Api side(std::move(config));
        ApplyRoute(side, active);
        side.max_output_tokens = kWebSearchMaxTokens;
        side.native_web_search = true;
        side.keep_abort = true;
        const auto started = std::chrono::steady_clock::now();
        ChatResult response = [&] {
          // The conversation sees this call's row, not the search's stream.
          QuietEvents quiet;
          return side.Chat(
              json::array({{{"role", "user"}, {"content", prompt}}}),
              json::array(),
              context.RemainingSeconds(WebSearchTimeoutSeconds() *
                                       kChatAttempts));
        }();
        if (response.interrupted || AbortRequested()) {
          return ToolCancelled("error: search cancelled by user");
        }
        Usage normalized;
        normalized.Add(response.usage);
        if (!normalized.web_searches && !response.content.empty()) {
          normalized.web_searches = 1;
        }
        usage.Add(RouteKey(active.base_url, "web_search", active.model,
                           active.reasoning_effort),
                  normalized, context.turn_id,
                  {{"model_calls", 1}, {"model_ms", ElapsedMs(started)}});
        if (context.Expired()) {
          return ToolTimedOut("error: search deadline reached");
        }
        if (!response.content.empty()) {
          std::string evidence = CitationEvidence(response.annotations);
          std::string output = std::move(response.content);
          if (!evidence.empty()) output += "\n\nSource evidence:\n" + evidence;
          ToolResult answer = ToolSuccess(std::move(output));
          answer.body_kind = "markdown";
          answer.hint =
              "[web search result; refetch only if verification is "
              "necessary]";
          if (response.stop_cause == ResponseStopCause::kLength) {
            AddHint(answer, "[truncated; narrow the query]");
          }
          return answer;
        }
        return SearchError(response.error.empty() ? "returned no answer"
                                                  : response.error);
      });
  t.capabilities = Capability(ToolCapability::kInspect) |
                   Capability(ToolCapability::kExternal);
  t.needs_approval = [](const json&) { return true; };
  t.headline = [](const json& a) {
    std::string target;
    if (const json* queries = JsonArray(a, "queries")) {
      for (const json& query : *queries) {
        if (!query.is_string()) continue;
        target +=
            (target.empty() ? "" : " | ") + FirstLine(query.get<std::string>());
      }
    }
    return Headline{"Searching the web for", "Searched the web for", target};
  };
  t.parallel_safe = true;
  t.parameters["properties"]["queries"]["maxItems"] = WebSearchMaxUses();
  // The request budget is per attempt; the tool deadline has to cover the
  // retries or it would cancel the call mid-recovery.
  t.timeout_s = WebSearchTimeoutSeconds() * kChatAttempts;
  t.max_calls_per_turn = WebSearchCalls();
  // Offered while a search can run on the route in use, which a model switch
  // changes.
  t.available = [&api, providers = std::move(providers)] {
    return SelectWebSearchRoute(api, providers).has_value();
  };
  return t;
}

}  // namespace uagent
