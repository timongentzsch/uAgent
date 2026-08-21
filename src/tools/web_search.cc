// Copyright 2026 Timon Gentzsch

#include "include/tools/web_search.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/api/citations.h"
#include "include/api/retry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

std::string DefaultSearchModel() {
  return EnvStr("OPENROUTER_MODEL", "openrouter/auto");
}

ToolResult SearchError(int64_t http_status, const std::string& detail) {
  std::string message = "error: web_search OpenRouter";
  if (http_status) message += " HTTP " + std::to_string(http_status);
  return ToolFailure(ToolErrorCode::kRemoteError, message + detail);
}

}  // namespace

WebSearchResult ParseWebSearch(const json& response) {
  WebSearchResult result;
  if (!response.is_object() || !response.contains("choices") ||
      !response["choices"].is_array() || response["choices"].empty()) {
    return result;
  }
  const json& choice = response["choices"][0];
  if (!choice.is_object() || !choice.contains("message") ||
      !choice["message"].is_object()) {
    return result;
  }
  const json& message = choice["message"];
  result.text = JsonValue(message, "content", "");
  result.annotations = JsonValue(message, "annotations", json::array());
  result.searches = 1;
  result.truncated = JsonValue(choice, "finish_reason", "") == "length";
  return result;
}

WebSearchRoute SelectWebSearchRoute(
    const Api& api, const std::vector<NamedProvider>& providers) {
  const RuntimeConfig& config = api.config;
  if (config.web_search_backend == "off") return {};

  // UAGENT_WEB_SEARCH_MODEL follows the shared selection schema. A provider
  // scope makes it a route of its own; a bare id only renames the model on
  // whichever candidate wins, which is how it behaved before the schema.
  ModelSelection selection = ParseModelSelection(config.web_search_model);
  if (selection.base.find('/') != std::string::npos) {
    ProviderCatalog catalog = SessionProviderCatalog();
    if (std::optional<ModelRoute> route = ResolveModelRoute(
            catalog.models, catalog.providers, selection.base)) {
      // An explicit selection is authoritative: a provider that does not speak
      // the OpenRouter protocol disables search rather than silently
      // redirecting the request to a different endpoint.
      if (route->protocol != ProviderProtocol::kOpenRouter) return {};
      return {route->base_url,
              route->api_key.empty() ? "sk-noop" : route->api_key, route->model,
              selection.effort};
    }
  }
  auto candidate = [&](std::string base_url, std::string api_key,
                       std::string model) {
    return WebSearchRoute{
        std::move(base_url), std::move(api_key),
        selection.base.empty() ? std::move(model) : selection.base,
        selection.effort};
  };

  // Most explicit first: a configured search endpoint, then the conversation's
  // own route, then any OpenRouter-protocol provider, then the built-in route.
  std::vector<WebSearchRoute> candidates;
  if (!config.web_search_url.empty() && !config.web_search_api_key.empty()) {
    candidates.push_back(candidate(StripTrailingSlashes(config.web_search_url),
                                   config.web_search_api_key,
                                   DefaultSearchModel()));
  }
  if (api.capabilities.OpenRouter()) {
    candidates.push_back(candidate(api.base_url, api.api_key, api.model));
  }
  for (const NamedProvider& provider : providers) {
    if (provider.protocol != ProviderProtocol::kOpenRouter) continue;
    candidates.push_back(
        candidate(provider.base_url, provider.api_key, DefaultSearchModel()));
    break;
  }
  if (std::string key = EnvStr("OPENROUTER_API_KEY"); !key.empty()) {
    candidates.push_back(candidate("https://openrouter.ai/api/v1",
                                   std::move(key), DefaultSearchModel()));
  }
  for (WebSearchRoute& winner : candidates) {
    if (winner.Valid()) return std::move(winner);
  }
  return {};
}

json WebSearchRequest(const WebSearchRoute& route, const RuntimeConfig& config,
                      const std::string& prompt) {
  // A `:effort` suffix on the selection is more specific than the session-wide
  // UAGENT_WEB_SEARCH_EFFORT default.
  const std::string& effort =
      route.effort.empty() ? config.web_search_effort : route.effort;
  json parameters = {{"engine", config.web_search_engine},
                     {"max_results", config.web_search_max_results},
                     {"max_total_results", config.web_search_max_results *
                                               config.web_search_max_uses},
                     {"max_uses", config.web_search_max_uses}};
  if (!config.web_search_context_size.empty()) {
    parameters["search_context_size"] = config.web_search_context_size;
  }
  json body = {
      {"model", route.model},
      {"stream", false},
      {"max_tokens", config.web_search_max_tokens},
      {"max_tool_calls", config.web_search_max_uses},
      {"usage", {{"include", true}}},
      {"tools", json::array({{{"type", "openrouter:web_search"},
                              {"parameters", std::move(parameters)}}})},
      {"messages", json::array({{{"role", "user"}, {"content", prompt}}})}};
  if (!effort.empty()) body["reasoning"] = {{"effort", effort}};
  return body;
}

Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                   std::vector<NamedProvider> providers) {
  Tool t = MakeTool(
      "web_search",
      "Search the web with cited sources; batch related queries up to the "
      "schema limit. Include dates or cutoffs in recency queries. Independent "
      "calls overlap. Do not repeat.",
      json::parse(R"json({"type":"object","properties":{
          "query":{"type":"string","description":"single query"},
          "queries":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":4,
            "description":"1-4 queries"}}})json"),
      [&api, &usage, providers = std::move(providers)](
          const json& a, const ToolContext& context) -> ToolResult {
        WebSearchRoute active = SelectWebSearchRoute(api, providers);
        if (!active.Valid()) {
          return ToolFailure(ToolErrorCode::kUnavailable,
                             "error: web_search is not configured");
        }
        std::vector<std::string> queries;
        if (a.contains("queries") && a["queries"].is_array()) {
          for (const json& value : a["queries"]) {
            if (!value.is_string()) continue;
            std::string query = Trim(value.get<std::string>());
            if (!query.empty()) queries.push_back(std::move(query));
          }
        }
        if (queries.empty()) {
          std::string query = Trim(JsonValue(a, "query", ""));
          if (!query.empty()) queries.push_back(std::move(query));
        }
        if (queries.empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: query or queries is required");
        }
        size_t max_queries = static_cast<size_t>(
            std::min<int64_t>(4, api.config.web_search_max_uses));
        if (queries.size() > max_queries) {
          return ToolFailure(ToolErrorCode::kLimitExceeded,
                             "error: too many queries for the configured "
                             "web search use limit");
        }
        std::string numbered;
        for (size_t i = 0; i < queries.size(); ++i) {
          numbered += std::to_string(i + 1) + ". " + queries[i] + "\n";
        }
        std::string prompt = "Current host date: " + LocalDay() +
                             ". Answer each numbered query concisely with "
                             "source URLs. For latest/newest claims, compare "
                             "publication dates from the official index. Use "
                             "only source-supported claims; preserve provider/"
                             "model scope and omit unasked pricing:\n" +
                             numbered;
        json body = WebSearchRequest(active, api.config, prompt);
        int64_t timeout =
            context.RemainingSeconds(api.config.web_search_timeout_s);
        auto started = std::chrono::steady_clock::now();
        DebugLog("side_request", {{"kind", "web_search"},
                                  {"path", "/chat/completions"},
                                  {"body", body}});
        Api side(api.config);
        side.base_url = active.base_url;
        side.api_key = active.api_key;
        JsonResponse response =
            side.Post("/chat/completions", body, timeout, kSideAttempts);
        DebugLog("side_response",
                 {{"kind", "web_search"},
                  {"duration_ms", ElapsedMs(started)},
                  {"cancelled", AbortRequested()},
                  {"http_status", response.http_status},
                  {"error", response.error},
                  {"response", response.body.is_discarded() ? json(nullptr)
                                                            : response.body}});
        if (AbortRequested()) {
          return ToolCancelled("error: search cancelled by user");
        }
        WebSearchResult result = ParseWebSearch(response.body);
        Usage normalized;
        if (response.body.is_object() && response.body.contains("usage")) {
          normalized.Add(response.body["usage"]);
        }
        if (!normalized.web_searches) {
          normalized.web_searches = result.searches;
        }
        result.searches = normalized.web_searches;
        if (normalized.web_searches > api.config.web_search_max_uses) {
          DebugLog("side_limit_exceeded",
                   {{"kind", "web_search"},
                    {"requested", api.config.web_search_max_uses},
                    {"reported", normalized.web_searches}});
        }
        if (response.body.is_object()) {
          const std::string& effort = active.effort.empty()
                                          ? api.config.web_search_effort
                                          : active.effort;
          usage.Add(
              RouteKey(active.base_url, "web_search", active.model, effort),
              normalized);
        }
        if (!result.text.empty()) {
          std::string evidence = CitationEvidence(result.annotations);
          std::string output =
              "[web search result; refetch only if verification is "
              "necessary]\n" +
              result.text;
          if (!evidence.empty()) output += "\n\nSource evidence:\n" + evidence;
          if (result.truncated) {
            output += "\n[truncated; raise UAGENT_WEB_SEARCH_MAX_TOKENS]";
          }
          return ToolSuccess(std::move(output));
        }
        if (response.body.is_object() && response.body.contains("error") &&
            response.body["error"].is_object()) {
          return SearchError(response.http_status,
                             ": " + JsonValue(response.body["error"], "message",
                                              "provider rejected the request"));
        }
        if (!response.error.empty()) {
          return SearchError(0, ": " + response.error);
        }
        return SearchError(0, " returned no answer");
      });
  t.capabilities = Capability(ToolCapability::kInspect) |
                   Capability(ToolCapability::kExternal);
  t.needs_approval = [](const json&) { return true; };
  t.summary = [](const json& a) {
    if (a.contains("queries") && a["queries"].is_array()) {
      std::string summary;
      for (const json& query : a["queries"]) {
        if (query.is_string()) {
          summary += (summary.empty() ? "" : " | ") +
                     FirstLine(query.get<std::string>());
        }
      }
      return summary;
    }
    return JsonValue(a, "query", "");
  };
  t.parallel_safe = true;
  t.parameters["properties"]["queries"]["maxItems"] =
      std::min<int64_t>(4, api.config.web_search_max_uses);
  // The configured budget is per attempt (see Api::Post); the tool deadline
  // has to cover the retries or it would cancel the call mid-recovery.
  int64_t budget = api.config.web_search_timeout_s;
  t.timeout_s = budget > std::numeric_limits<int64_t>::max() / kSideAttempts
                    ? std::numeric_limits<int64_t>::max()
                    : budget * kSideAttempts;
  t.max_calls_per_turn = api.config.web_search_calls;
  return t;
}

}  // namespace uagent
