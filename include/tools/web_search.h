// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
#define UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
// OpenRouter-compatible web search, billed only when the model chooses to call
// it — unlike the /online toggle, which pays on every request.

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

struct WebSearchRoute {
  std::string base_url;
  std::string api_key;
  std::string model;

  bool Valid() const {
    return !base_url.empty() && !api_key.empty() && !model.empty();
  }
};

// Lets the model reach the web through an OpenRouter-compatible endpoint via a
// quiet side-request to <model>:online. Costs one search-enabled
// completion per call — but only when actually used, unlike the /online
// toggle which pays on every request.
inline Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                          WebSearchRoute fallback = {}) {
  Tool t = MakeTool(
      "web_search",
      "Search OpenRouter with URLs; batch up to four queries. Independent "
      "calls overlap. Do not repeat.",
      json::parse(R"json({"type":"object","properties":{
          "query":{"type":"string","description":"single query"},
          "queries":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":4,
            "description":"1-4 queries"}}})json"),
      [&api, &usage, fallback = std::move(fallback)](
          const json& a, const ToolContext& context) -> ToolResult {
        bool active_openrouter = OpenrouterUrl(api.base_url);
        WebSearchRoute route =
            active_openrouter
                ? WebSearchRoute{api.base_url, api.api_key, api.model}
                : fallback;
        if (!route.Valid()) {
          return ToolFailure(ToolErrorCode::kUnavailable,
                             "error: web_search requires an OpenRouter route");
        }
        std::vector<std::string> queries;
        if (a.contains("queries") && a["queries"].is_array()) {
          for (const json& value : a["queries"]) {
            if (value.is_string() && !Trim(value.get<std::string>()).empty()) {
              queries.push_back(Trim(value.get<std::string>()));
            }
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
        if (queries.size() > 4) {
          return ToolFailure(ToolErrorCode::kLimitExceeded,
                             "error: queries is limited to four items");
        }
        std::string query;
        for (size_t i = 0; i < queries.size(); ++i) {
          query += std::to_string(i + 1) + ". " + queries[i] + "\n";
        }
        RuntimeConfig config = api.config;
        std::string base = config.web_search_model.empty()
                               ? route.model
                               : config.web_search_model;
        base = base.substr(0, base.find(':'));
        json body = {
            {"model", base + ":online"},
            {"max_tokens", config.web_search_max_tokens},
            {"usage", {{"include", true}}},  // OpenRouter: report cost
            {"messages",
             json::array(
                 {{{"role", "user"},
                   {"content",
                    "Answer each numbered query concisely with source URLs. "
                    "Use only source-supported claims; preserve provider/model "
                    "scope and omit unasked pricing:\n" +
                        query}}})}};
        if (!config.web_search_effort.empty()) {
          body["reasoning"] = {{"effort", config.web_search_effort}};
        }
        int64_t timeout = context.RemainingSeconds(config.web_search_timeout_s);
        auto started = std::chrono::steady_clock::now();
        DebugLog("side_request", {{"kind", "web_search"},
                                  {"path", "/chat/completions"},
                                  {"body", body}});
        Api side(config);
        side.base_url = std::move(route.base_url);
        side.api_key = std::move(route.api_key);
        json r = side.Post("/chat/completions", body, timeout);
        DebugLog("side_response",
                 {{"kind", "web_search"},
                  {"duration_ms", ElapsedMs(started)},
                  {"cancelled", AbortRequested()},
                  {"response", r.is_discarded() ? json(nullptr) : r}});
        if (AbortRequested()) {
          return ToolCancelled("error: search cancelled by user");
        }
        if (r.is_object() && r.contains("usage")) usage.Add(r["usage"]);
        if (r.is_object() && r.contains("choices") && r["choices"].is_array() &&
            !r["choices"].empty() && r["choices"][0].is_object()) {
          const json& choice = r["choices"][0];
          std::string content = "(empty answer)";
          if (choice.contains("message") && choice["message"].is_object()) {
            content = JsonString(choice["message"], "content", content);
          }
          std::string output =
              "[web search result; refetch only if verification is "
              "necessary]\n" +
              content;
          if (JsonString(choice, "finish_reason") == "length") {
            output += "\n[truncated; raise UAGENT_WEB_SEARCH_MAX_TOKENS]";
          }
          return ToolSuccess(std::move(output));
        }
        if (r.is_object() && r.contains("error") && r["error"].is_object()) {
          return ToolFailure(
              ToolErrorCode::kRemoteError,
              "error: " + JsonString(r["error"], "message", "search failed"));
        }
        return ToolFailure(ToolErrorCode::kRemoteError,
                           "error: web search failed");
      });
  t.mutating = true;
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
  t.max_calls_per_turn = api.config.web_search_calls;
  return t;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
