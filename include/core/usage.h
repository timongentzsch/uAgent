// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_USAGE_H_
#define UAGENT_INCLUDE_CORE_USAGE_H_

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
    auto detail = [&](const char* key, const char* field) {
      return value.contains(key) && value[key].is_object()
                 ? JsonValue(value[key], field, int64_t{0})
                 : int64_t{0};
    };
    int64_t cache = detail("prompt_tokens_details", "cached_tokens");
    if (!cache) cache = detail("input_tokens_details", "cached_tokens");
    int64_t cache_write_tokens =
        detail("prompt_tokens_details", "cache_write_tokens");
    if (!cache_write_tokens) {
      cache_write_tokens = detail("cache_details", "cache_write_tokens");
    }
    if (!cache_write_tokens) {
      cache_write_tokens = JsonValue(value, "cache_write_tokens", int64_t{0});
    }
    int64_t reason = detail("completion_tokens_details", "reasoning_tokens");
    if (!reason) reason = detail("output_tokens_details", "reasoning_tokens");
    int64_t input_tokens = JsonValue(value, "prompt_tokens", int64_t{0});
    if (!input_tokens) {
      input_tokens = JsonValue(value, "input_tokens", int64_t{0});
    }
    int64_t output_tokens = JsonValue(value, "completion_tokens", int64_t{0});
    if (!output_tokens) {
      output_tokens = JsonValue(value, "output_tokens", int64_t{0});
    }
    input += input_tokens - cache;
    output += output_tokens - reason;
    cache_read += cache;
    cache_write += cache_write_tokens;
    reasoning += reason;
    if (value.contains("cost") && value["cost"].is_number()) {
      cost += value["cost"].get<double>();
      cost_reported = true;
    }
    if (value.contains("server_tool_use") &&
        value["server_tool_use"].is_object()) {
      web_searches += JsonValue(value["server_tool_use"], "web_search_requests",
                                int64_t{0});
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
