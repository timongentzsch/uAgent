// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_USAGE_H_
#define UAGENT_INCLUDE_CORE_USAGE_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "include/core/json.h"

namespace uagent {

using nlohmann::json;

struct Usage {
  int64_t input = 0;
  int64_t output = 0;
  int64_t cache_read = 0;
  int64_t reasoning = 0;
  double cost = 0;
  bool cost_reported = false;
  int64_t cache_write = 0;
  int64_t web_searches = 0;

  int64_t GeneratedTokens() const { return output + reasoning; }

  // `input` excludes the cached part, so the two together are the whole
  // prompt. Zero when nothing has been counted yet.
  int64_t CacheHitPercent() const {
    int64_t prompt = input + cache_read;
    return prompt > 0 ? 100 * cache_read / prompt : 0;
  }

  void Merge(const Usage& other) {
    input += other.input;
    output += other.output;
    cache_read += other.cache_read;
    reasoning += other.reasoning;
    cost += other.cost;
    cost_reported = cost_reported || other.cost_reported;
    cache_write += other.cache_write;
    web_searches += other.web_searches;
  }

  // OpenAI convention: input excludes cached tokens, output excludes reasoning.
  void Add(const json& value) {
    if (!value.is_object()) return;
    // OpenAI-compatible endpoints spell the same counts several ways. Each
    // field lists its spellings in order and takes the first one present,
    // rather than growing another chain of fallbacks per field.
    struct Alias {
      const char* parent;  // nullptr for a top-level field
      const char* field;
    };
    auto first = [&](std::initializer_list<Alias> candidates) {
      for (const Alias& alias : candidates) {
        int64_t found =
            alias.parent
                ? (value.contains(alias.parent) &&
                           value[alias.parent].is_object()
                       ? JsonValue(value[alias.parent], alias.field, int64_t{0})
                       : int64_t{0})
                : JsonValue(value, alias.field, int64_t{0});
        if (found) return found;
      }
      return int64_t{0};
    };

    int64_t input_tokens =
        first({{nullptr, "prompt_tokens"}, {nullptr, "input_tokens"}});
    int64_t output_tokens =
        first({{nullptr, "completion_tokens"}, {nullptr, "output_tokens"}});
    int64_t reason = first({{"completion_tokens_details", "reasoning_tokens"},
                            {"output_tokens_details", "reasoning_tokens"}});
    // Chat Completions and Responses report cached tokens *inside* the prompt
    // total, so they must be subtracted out. Anthropic-style usage reports
    // them beside an input count that already excludes them — subtracting
    // there would under-report the fresh tokens.
    int64_t nested_cache = first({{"prompt_tokens_details", "cached_tokens"},
                                  {"input_tokens_details", "cached_tokens"}});
    int64_t cache =
        nested_cache ? nested_cache
                     : JsonValue(value, "cache_read_input_tokens", int64_t{0});
    int64_t cache_write_tokens =
        first({{"prompt_tokens_details", "cache_write_tokens"},
               {"prompt_tokens_details", "cache_creation_tokens"},
               {"cache_details", "cache_write_tokens"},
               {nullptr, "cache_write_tokens"},
               {nullptr, "cache_creation_input_tokens"}});
    // Compatibility providers occasionally report detail counts larger than
    // their parent totals. Never surface impossible negative token counts.
    input += nested_cache ? std::max(int64_t{0}, input_tokens - nested_cache)
                          : input_tokens;
    output += std::max(int64_t{0}, output_tokens - reason);
    cache_read += cache;
    cache_write += cache_write_tokens;
    reasoning += reason;
    if (value.contains("cost") && value["cost"].is_number()) {
      cost += value["cost"].get<double>();
      cost_reported = true;
    }
    const json* server_tools = nullptr;
    if (value.contains("server_tool_use_details") &&
        value["server_tool_use_details"].is_object()) {
      server_tools = &value["server_tool_use_details"];
    } else if (value.contains("server_tool_use") &&
               value["server_tool_use"].is_object()) {
      server_tools = &value["server_tool_use"];
    }
    if (server_tools) {
      web_searches +=
          JsonValue(*server_tools, "web_search_requests", int64_t{0});
    }
  }
};

inline json UsageJson(const Usage& usage) {
  return {{"input", usage.input},
          {"output", usage.output},
          {"cache_read", usage.cache_read},
          {"cache_write", usage.cache_write},
          {"reasoning", usage.reasoning},
          {"cost", usage.cost},
          {"cost_reported", usage.cost_reported},
          {"web_searches", usage.web_searches}};
}

inline Usage UsageFromJson(const json& value) {
  Usage usage;
  if (!value.is_object()) return usage;
  usage.input = JsonValue(value, "input", int64_t{0});
  usage.output = JsonValue(value, "output", int64_t{0});
  usage.cache_read = JsonValue(value, "cache_read", int64_t{0});
  usage.cache_write = JsonValue(value, "cache_write", int64_t{0});
  usage.reasoning = JsonValue(value, "reasoning", int64_t{0});
  usage.cost = JsonValue(value, "cost", 0.0);
  usage.cost_reported = JsonValue(value, "cost_reported", usage.cost != 0);
  usage.web_searches = JsonValue(value, "web_searches", int64_t{0});
  return usage;
}

using RouteUsage = std::map<std::string, Usage>;

inline json RouteUsageJson(const RouteUsage& routes) {
  json out = json::object();
  for (const auto& [route, usage] : routes) out[route] = UsageJson(usage);
  return out;
}

inline RouteUsage RouteUsageFromJson(const json& value) {
  RouteUsage routes;
  if (!value.is_object()) return routes;
  for (const auto& [route, usage] : value.items()) {
    routes[route] = UsageFromJson(usage);
  }
  return routes;
}

class UsageAccumulator {
 public:
  void Add(const json& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    usage_.Add(usage);
  }

  void Add(const Usage& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    usage_.Merge(usage);
  }

  void Add(const std::string& route, const Usage& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    usage_.Merge(usage);
    routes_[route].Merge(usage);
  }

  Usage Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Usage usage = usage_;
    usage_ = {};
    return usage;
  }

  RouteUsage TakeRoutes() {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteUsage routes;
    routes.swap(routes_);
    return routes;
  }

 private:
  std::mutex mutex_;
  Usage usage_;
  RouteUsage routes_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_USAGE_H_
