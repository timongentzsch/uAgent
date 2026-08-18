// Copyright 2026 Timon Gentzsch

#include "include/tools/web_search.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "include/api/citations.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

WebSearchResult ParseOpenRouterSearch(const json& response) {
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

void AddResponseAnnotations(const json& annotations, WebSearchResult& result) {
  if (!annotations.is_array()) return;
  for (const json& annotation : annotations) {
    if (annotation.is_object()) result.annotations.push_back(annotation);
  }
}

}  // namespace

WebSearchRoute SelectWebSearchRoute(
    const Api& api, const std::vector<NamedProvider>& providers) {
  const RuntimeConfig& config = api.config;
  if (config.web_search_backend == "off") return {};

  std::optional<WebSearchBackend> wanted;
  if (config.web_search_backend == "responses") {
    wanted = WebSearchBackend::kResponses;
  } else if (config.web_search_backend == "openrouter") {
    wanted = WebSearchBackend::kOpenRouter;
  }
  auto backend_for = [](ProviderProtocol protocol, const std::string& url) {
    return CapabilitiesForRoute(protocol, url).search_protocol ==
                   SearchProtocol::kOpenRouter
               ? WebSearchBackend::kOpenRouter
               : WebSearchBackend::kResponses;
  };
  auto find_provider = [&](SearchProtocol protocol) -> const NamedProvider* {
    auto found = std::find_if(
        providers.begin(), providers.end(), [&](const NamedProvider& provider) {
          return CapabilitiesForRoute(provider.protocol, provider.base_url)
                     .search_protocol == protocol;
        });
    return found == providers.end() ? nullptr : &*found;
  };

  // UAGENT_WEB_SEARCH_MODEL follows the shared selection schema. A provider
  // scope makes it a route of its own; a bare id only renames the model on
  // whichever candidate wins, which is how it behaved before the schema.
  ModelSelection selection = ParseModelSelection(config.web_search_model);
  std::optional<ModelRoute> selected;
  if (selection.base.find('/') != std::string::npos) {
    ProviderCatalog catalog = SessionProviderCatalog();
    selected = ResolveModelRoute(catalog.models, catalog.providers,
                                 selection.base);
  }
  auto with_model = [&](WebSearchRoute route) {
    if (!selection.base.empty() && !selected) route.model = selection.base;
    route.effort = selection.effort;
    return route;
  };

  // Most explicit first. The first valid candidate that matches an explicitly
  // requested backend wins; with none requested, the first valid one wins.
  std::vector<WebSearchRoute> candidates;
  if (selected) {
    candidates.push_back(
        {backend_for(selected->protocol, selected->base_url),
         selected->base_url,
         selected->api_key.empty() ? "sk-noop" : selected->api_key,
         selected->model, selection.effort});
  }
  if (!config.web_search_url.empty() && !config.web_search_api_key.empty()) {
    std::string url = StripTrailingSlashes(config.web_search_url);
    WebSearchBackend backend =
        wanted.value_or(OpenrouterUrl(url) ? WebSearchBackend::kOpenRouter
                                           : WebSearchBackend::kResponses);
    std::string model = backend == WebSearchBackend::kOpenRouter
                            ? EnvStr("OPENROUTER_MODEL", "openrouter/auto")
                            : EnvStr("OPENAI_MODEL", "gpt-5.6");
    candidates.push_back(with_model({backend, std::move(url),
                                     config.web_search_api_key,
                                     std::move(model), std::string()}));
  }
  if (!api.base_url.empty() && !api.api_key.empty() &&
      api.capabilities.search_protocol != SearchProtocol::kNone) {
    candidates.push_back(with_model(
        {backend_for(api.capabilities.protocol, api.base_url), api.base_url,
         api.api_key, api.model, std::string()}));
  }
  for (SearchProtocol protocol :
       {SearchProtocol::kResponses, SearchProtocol::kOpenRouter}) {
    const NamedProvider* provider = find_provider(protocol);
    if (!provider) continue;
    candidates.push_back(with_model(
        {backend_for(provider->protocol, provider->base_url),
         provider->base_url, provider->api_key,
         protocol == SearchProtocol::kOpenRouter
             ? EnvStr("OPENROUTER_MODEL", "openrouter/auto")
             : EnvStr("OPENAI_MODEL", "gpt-5.6"),
         std::string()}));
  }
  if (std::string key = EnvStr("OPENAI_API_KEY"); !key.empty()) {
    candidates.push_back(with_model({WebSearchBackend::kResponses,
                                     "https://api.openai.com/v1",
                                     std::move(key),
                                     EnvStr("OPENAI_MODEL", "gpt-5.6"),
                                     std::string()}));
  }

  for (WebSearchRoute& candidate : candidates) {
    if (!candidate.Valid()) continue;
    if (wanted && candidate.backend != *wanted) continue;
    return std::move(candidate);
  }
  return {};
}

WebSearchResult ParseResponsesSearch(const json& response) {
  WebSearchResult result;
  if (!response.is_object() || !response.contains("output") ||
      !response["output"].is_array()) {
    return result;
  }
  for (const json& item : response["output"]) {
    if (!item.is_object()) continue;
    if (JsonValue(item, "type", "") == "web_search_call") {
      ++result.searches;
      if (item.contains("action") && item["action"].is_object() &&
          item["action"].contains("sources")) {
        AddResponseAnnotations(item["action"]["sources"], result);
      }
      continue;
    }
    if (JsonValue(item, "type", "") != "message" || !item.contains("content") ||
        !item["content"].is_array()) {
      continue;
    }
    for (const json& part : item["content"]) {
      if (!part.is_object() || JsonValue(part, "type", "") != "output_text") {
        continue;
      }
      if (!result.text.empty()) result.text += "\n";
      result.text += JsonValue(part, "text", "");
      AddResponseAnnotations(JsonValue(part, "annotations", json::array()),
                             result);
    }
  }
  result.truncated = JsonValue(response, "status", "") == "incomplete";
  return result;
}

json WebSearchRequest(const WebSearchRoute& route, const RuntimeConfig& config,
                      const std::string& prompt) {
  // A `:effort` suffix on the selection is more specific than the session-wide
  // UAGENT_WEB_SEARCH_EFFORT default.
  const std::string& effort =
      route.effort.empty() ? config.web_search_effort : route.effort;
  if (route.backend == WebSearchBackend::kResponses) {
    json tool = {{"type", "web_search"}};
    if (!config.web_search_context_size.empty()) {
      tool["search_context_size"] = config.web_search_context_size;
    }
    json body = {{"model", route.model},
                 {"input", prompt},
                 {"stream", false},
                 {"tools", json::array({std::move(tool)})},
                 {"include", json::array({"web_search_call.action.sources"})},
                 {"max_output_tokens", config.web_search_max_tokens},
                 {"max_tool_calls", config.web_search_max_uses},
                 {"store", false}};
    if (!effort.empty()) body["reasoning"] = {{"effort", effort}};
    return body;
  }
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
        const char* path = active.backend == WebSearchBackend::kResponses
                               ? "/responses"
                               : "/chat/completions";
        int64_t timeout =
            context.RemainingSeconds(api.config.web_search_timeout_s);
        auto started = std::chrono::steady_clock::now();
        DebugLog("side_request",
                 {{"kind", "web_search"},
                  {"backend", active.backend == WebSearchBackend::kResponses
                                  ? "responses"
                                  : "openrouter"},
                  {"path", path},
                  {"body", body}});
        Api side(api.config);
        side.base_url = active.base_url;
        side.api_key = active.api_key;
        JsonResponse response = side.Post(path, body, timeout);
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
        WebSearchResult result = active.backend == WebSearchBackend::kResponses
                                     ? ParseResponsesSearch(response.body)
                                     : ParseOpenRouterSearch(response.body);
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
          usage.Add(RouteKey(active.base_url, "web_search", active.model,
                             effort),
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
        std::string backend = active.backend == WebSearchBackend::kResponses
                                  ? "Responses"
                                  : "OpenRouter";
        if (response.body.is_object() && response.body.contains("error") &&
            response.body["error"].is_object()) {
          std::string prefix = "error: web_search " + backend;
          if (response.http_status) {
            prefix += " HTTP " + std::to_string(response.http_status);
          }
          return ToolFailure(ToolErrorCode::kRemoteError,
                             prefix + ": " +
                                 JsonValue(response.body["error"], "message",
                                           "provider rejected the request"));
        }
        if (!response.error.empty()) {
          return ToolFailure(
              ToolErrorCode::kRemoteError,
              "error: web_search " + backend + ": " + response.error);
        }
        return ToolFailure(
            ToolErrorCode::kRemoteError,
            "error: web_search " + backend + " returned no answer");
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
  t.timeout_s = api.config.web_search_timeout_s;
  t.max_calls_per_turn = api.config.web_search_calls;
  return t;
}

}  // namespace uagent
