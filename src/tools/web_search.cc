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

std::string SelectedWebSearchModel(const RuntimeConfig& config,
                                   const std::string& fallback) {
  return config.web_search_model.empty() ? fallback : config.web_search_model;
}

WebSearchRoute ProviderSearchRoute(WebSearchBackend backend,
                                   const NamedProvider* provider,
                                   const RuntimeConfig& config,
                                   std::string default_model) {
  if (!provider) return {};
  return {backend, provider->base_url, provider->api_key,
          SelectedWebSearchModel(config, default_model)};
}

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
  const NamedProvider* openai = FindNamedProvider(providers, "openai");
  const NamedProvider* openrouter = FindNamedProvider(providers, "openrouter");
  std::string openai_model = EnvStr("OPENAI_MODEL", "gpt-5.6");
  std::string openrouter_model = EnvStr("OPENROUTER_MODEL", "openrouter/auto");

  auto active = [&](WebSearchBackend backend, std::string model) {
    if (api.base_url.empty() || api.api_key.empty()) return WebSearchRoute{};
    return WebSearchRoute{backend, api.base_url, api.api_key,
                          SelectedWebSearchModel(config, model)};
  };
  auto explicit_route = [&](WebSearchBackend backend) {
    if (config.web_search_url.empty() || config.web_search_api_key.empty()) {
      return WebSearchRoute{};
    }
    std::string model = backend == WebSearchBackend::kOpenRouter
                            ? openrouter_model
                            : openai_model;
    return WebSearchRoute{backend, StripTrailingSlashes(config.web_search_url),
                          config.web_search_api_key,
                          SelectedWebSearchModel(config, model)};
  };
  auto environment_openai = [&] {
    std::string key = EnvStr("OPENAI_API_KEY");
    if (key.empty()) return WebSearchRoute{};
    return WebSearchRoute{WebSearchBackend::kResponses,
                          "https://api.openai.com/v1", std::move(key),
                          SelectedWebSearchModel(config, openai_model)};
  };

  // Explicit search configuration wins. Otherwise prefer a compatible active
  // route, then independent OpenAI and OpenRouter credentials in that order.
  if (config.web_search_backend == "responses") {
    WebSearchRoute route = explicit_route(WebSearchBackend::kResponses);
    if (!route.Valid() && OpenaiUrl(api.base_url)) {
      return active(WebSearchBackend::kResponses, api.model);
    }
    if (!route.Valid()) {
      route = ProviderSearchRoute(WebSearchBackend::kResponses, openai, config,
                                  openai_model);
    }
    return route.Valid() ? route : environment_openai();
  }
  if (config.web_search_backend == "openrouter") {
    if (WebSearchRoute route = explicit_route(WebSearchBackend::kOpenRouter);
        route.Valid()) {
      return route;
    }
    if (api.openrouter_compatible) {
      return active(WebSearchBackend::kOpenRouter, api.model);
    }
    return ProviderSearchRoute(WebSearchBackend::kOpenRouter, openrouter,
                               config, openrouter_model);
  }

  if (!config.web_search_url.empty() && !config.web_search_api_key.empty()) {
    WebSearchBackend backend = OpenrouterUrl(config.web_search_url)
                                   ? WebSearchBackend::kOpenRouter
                                   : WebSearchBackend::kResponses;
    return explicit_route(backend);
  }
  if (api.openrouter_compatible) {
    return active(WebSearchBackend::kOpenRouter, api.model);
  }
  if (OpenaiUrl(api.base_url)) {
    return active(WebSearchBackend::kResponses, api.model);
  }
  WebSearchRoute route = ProviderSearchRoute(WebSearchBackend::kResponses,
                                             openai, config, openai_model);
  if (!route.Valid()) {
    route = ProviderSearchRoute(WebSearchBackend::kOpenRouter, openrouter,
                                config, openrouter_model);
  }
  return route.Valid() ? route : environment_openai();
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
    if (!config.web_search_effort.empty()) {
      body["reasoning"] = {{"effort", config.web_search_effort}};
    }
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
  if (!config.web_search_effort.empty()) {
    body["reasoning"] = {{"effort", config.web_search_effort}};
  }
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
          usage.Add(RouteKey(active.base_url, "web_search", active.model,
                             api.config.web_search_effort),
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
