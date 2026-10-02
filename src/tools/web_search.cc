// Copyright 2026 Timon Gentzsch

#include "include/tools/web_search.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/api/citations.h"
#include "include/api/retry.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/providers.h"

namespace uagent {
namespace {

std::string DefaultSearchModel() {
  return EnvStr("OPENROUTER_MODEL", kDefaultModelRoute);
}

ToolResult SearchError(int64_t http_status, const std::string& detail) {
  std::string message = "error: web_search OpenRouter";
  if (http_status) message += " HTTP " + std::to_string(http_status);
  return ToolFailure(ToolErrorCode::kRemoteError, message + detail);
}

}  // namespace

WebSearchResult ParseWebSearch(const json& response) {
  WebSearchResult result;
  const json* choices = JsonArray(response, "choices");
  if (!choices || choices->empty()) return result;
  const json& choice = choices->front();
  const json* found = JsonObject(choice, "message");
  if (!found) return result;
  const json& message = *found;
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
              route->api_key.empty() ? kPlaceholderApiKey : route->api_key,
              route->model, selection.effort};
    }
  }
  auto candidate = [&](std::string base_url, std::string api_key,
                       std::string model) {
    return WebSearchRoute{
        std::move(base_url), std::move(api_key),
        selection.base.empty() ? std::move(model) : selection.base,
        selection.effort};
  };

  // The conversation's own route, then any OpenRouter-protocol provider, then
  // the built-in route.
  std::vector<WebSearchRoute> candidates;
  if (api.capabilities.OpenRouter()) {
    candidates.push_back(candidate(api.base_url, api.api_key, api.model));
  }
  for (const NamedProvider& provider : providers) {
    if (provider.protocol != ProviderProtocol::kOpenRouter) continue;
    candidates.push_back(
        candidate(provider.base_url, provider.api_key, DefaultSearchModel()));
    break;
  }
  if (std::string key = SettingText(Cfg("OPENROUTER_API_KEY")); !key.empty()) {
    candidates.push_back(candidate("https://openrouter.ai/api/v1",
                                   std::move(key), DefaultSearchModel()));
  }
  for (WebSearchRoute& winner : candidates) {
    if (winner.Valid()) return std::move(winner);
  }
  return {};
}

json WebSearchRequest(const WebSearchRoute& route, const std::string& prompt) {
  json parameters = {
      {"engine", "auto"},
      {"max_results", kWebSearchMaxResults},
      {"max_total_results", kWebSearchMaxResults * kWebSearchMaxUses},
      {"max_uses", kWebSearchMaxUses}};
  json body = {
      {"model", route.model},
      {"stream", false},
      {"max_tokens", kWebSearchMaxTokens},
      {"max_tool_calls", kWebSearchMaxUses},
      {"usage", {{"include", true}}},
      {"tools", json::array({{{"type", "openrouter:web_search"},
                              {"parameters", std::move(parameters)}}})},
      {"messages", json::array({{{"role", "user"}, {"content", prompt}}})}};
  if (!route.effort.empty()) body["reasoning"] = {{"effort", route.effort}};
  return body;
}

Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                   std::vector<NamedProvider> providers) {
  Tool t = MakeTool(
      "web_search",
      "Search the web with cited sources; put up to 4 related queries in one "
      "call. Include dates or cutoffs in recency queries. Do not repeat a "
      "search.",
      json::parse(R"json({"type":"object","properties":{
          "queries":{"type":"array","items":{"type":"string","minLength":1},
            "minItems":1,"maxItems":4}},
          "required":["queries"]})json"),
      [&api, &usage, providers = std::move(providers)](
          const json& a, const ToolContext& context) -> ToolResult {
        WebSearchRoute active = SelectWebSearchRoute(api, providers);
        if (!active.Valid()) {
          return ToolFailure(ToolErrorCode::kUnavailable,
                             "web_search is not configured");
        }
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
        if (queries.size() > static_cast<size_t>(kWebSearchMaxUses)) {
          return ToolFailure(ToolErrorCode::kLimitExceeded,
                             "too many queries for the configured "
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
        json body = WebSearchRequest(active, prompt);
        int64_t timeout = context.RemainingSeconds(kWebSearchTimeoutSeconds);
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
        if (normalized.web_searches > kWebSearchMaxUses) {
          DebugLog("side_limit_exceeded",
                   {{"kind", "web_search"},
                    {"requested", kWebSearchMaxUses},
                    {"reported", normalized.web_searches}});
        }
        if (response.body.is_object()) {
          usage.Add(
              RouteKey(active.base_url, "web_search", active.model,
                       active.effort),
              normalized, context.turn_id,
              {{"model_calls", 1},
               {"model_ms", ElapsedMs(started)},
               {"usage_samples", response.body.contains("usage") ? 1 : 0}});
        }
        if (!result.text.empty()) {
          std::string evidence = CitationEvidence(result.annotations);
          std::string output =
              "[web search result; refetch only if verification is "
              "necessary]\n" +
              result.text;
          if (!evidence.empty()) output += "\n\nSource evidence:\n" + evidence;
          if (result.truncated) {
            output += "\n[truncated; narrow the query]";
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
    auto queries = a.find("queries");
    if (queries == a.end() || !queries->is_array()) return std::string();
    std::string summary;
    for (const json& query : *queries) {
      if (!query.is_string()) continue;
      summary +=
          (summary.empty() ? "" : " | ") + FirstLine(query.get<std::string>());
    }
    return summary;
  };
  t.parallel_safe = true;
  t.header = Verbs("Searching the web for", "Searched the web for");
  t.intent = "research";
  t.parameters["properties"]["queries"]["maxItems"] = kWebSearchMaxUses;
  // The request budget is per attempt (see Api::Post); the tool deadline has
  // to cover the retries or it would cancel the call mid-recovery.
  t.timeout_s = kWebSearchTimeoutSeconds * kSideAttempts;
  t.max_calls_per_turn = kWebSearchCalls;
  return t;
}

}  // namespace uagent
