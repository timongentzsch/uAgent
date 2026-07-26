// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
#define UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
// OpenRouter-only web search as a tool, billed only when the model chooses
// to call it — unlike the /online toggle, which pays on every request.

#include <algorithm>
#include <atomic>
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

// OpenRouter-only: lets the model reach the web when IT decides it needs to,
// via a quiet side-request to <model>:online. Costs one search-enabled
// completion per call — but only when actually used, unlike the /online
// toggle which pays on every request.
inline Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                          SideTaskSupervisor& side_tasks) {
  Tool t = MakeTool(
      "web_search",
      "Search via OpenRouter with source URLs. Batch up to four queries. Slow "
      "searches "
      "background automatically; continue useful work and join only when "
      "needed. "
      "Do not repeat.",
      json::parse(R"json({"type":"object","properties":{
          "query":{"type":"string","description":"one query (legacy shorthand)"},
          "queries":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":4,
            "description":"one to four queries in one request"}}})json"),
      [&api, &usage, &side_tasks](const json& a,
                                  const ToolContext& context) -> std::string {
        if (!OpenrouterUrl(api.base_url)) {
          return "error: web_search is available only for OpenRouter";
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
        if (queries.empty()) return "error: query or queries is required";
        if (queries.size() > 4) {
          return "error: queries is limited to four items";
        }
        std::string query;
        for (size_t i = 0; i < queries.size(); ++i) {
          query += std::to_string(i + 1) + ". " + queries[i] + "\n";
        }
        const std::string base_url = api.base_url, api_key = api.api_key;
        const std::string model = api.model;
        RuntimeConfig config = api.config;
        std::string base =
            config.web_search_model.empty() ? model : config.web_search_model;
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
        int64_t timeout =
            std::max(config.web_search_timeout_s, context.timeout_s);
        int64_t id = side_tasks.Start(
            "web search", query,
            [base_url, api_key, config, body = std::move(body), timeout,
             &usage](const std::atomic<bool>& cancel) {
              auto started = std::chrono::steady_clock::now();
              DebugLog("side_request", {{"kind", "web_search"},
                                        {"path", "/chat/completions"},
                                        {"body", body}});
              Api side(config);
              side.base_url = base_url;
              side.api_key = api_key;
              json r = side.Post("/chat/completions", body, timeout, &cancel);
              DebugLog("side_response",
                       {{"kind", "web_search"},
                        {"duration_ms", ElapsedMs(started)},
                        {"cancelled", cancel.load()},
                        {"response", r.is_discarded() ? json(nullptr) : r}});
              if (cancel.load()) {
                return std::string("error: web search abandoned");
              }
              if (AbortRequested()) {
                return std::string("error: search cancelled by user");
              }
              if (r.is_object() && r.contains("usage")) usage.Add(r["usage"]);
              if (r.is_object() && r.contains("choices") &&
                  r["choices"].is_array() && !r["choices"].empty() &&
                  r["choices"][0].is_object()) {
                const json& choice = r["choices"][0];
                std::string content = "(empty answer)";
                if (choice.contains("message") &&
                    choice["message"].is_object()) {
                  content = JsonString(choice["message"], "content", content);
                }
                std::string output =
                    "[web search result; refetch only if verification is "
                    "necessary]\n" +
                    content;
                if (JsonString(choice, "finish_reason") == "length") {
                  output += "\n[truncated; raise UAGENT_WEB_SEARCH_MAX_TOKENS]";
                }
                return output;
              }
              if (r.is_object() && r.contains("error") &&
                  r["error"].is_object()) {
                return "error: " +
                       JsonString(r["error"], "message", "search failed");
              }
              return std::string("error: web search failed");
            },
            ToolConcurrency());
        if (!id) return "error: concurrent side-task limit reached";
        int64_t grace = context.timeout_s == 0
                            ? timeout
                            : std::min(context.timeout_s, timeout);
        if (auto result = side_tasks.Wait(id, std::chrono::seconds(grace))) {
          return result->output;
        }
        DebugLog("side_backgrounded",
                 {{"kind", "web_search"}, {"id", id}, {"query", query}});
        return "[backgrounded] web search job " + std::to_string(id) +
               "; continue other work or call wait_background(id=" +
               std::to_string(id) + ")";
      });
  t.mutating = true;
  t.summary = [](const json& a) {
    if (a.contains("queries") && a["queries"].is_array()) {
      std::string summary;
      for (const json& query : a["queries"]) {
        if (query.is_string()) {
          summary += (summary.empty() ? "" : " | ") +
                     OneLine(query.get<std::string>(), 60);
        }
      }
      return summary;
    }
    return JsonValue(a, "query", "");
  };
  t.parallel_safe = true;
  t.timeout_s = 5;
  t.max_calls_per_turn = api.config.web_search_calls;
  return t;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_WEB_SEARCH_H_
