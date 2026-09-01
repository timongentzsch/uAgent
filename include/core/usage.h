// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_USAGE_H_
#define UAGENT_INCLUDE_CORE_USAGE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <string>

#include "include/core/checked.h"
#include "include/core/json.h"

namespace uagent {

struct Usage {
  int64_t input = 0;
  int64_t output = 0;
  int64_t cache_read = 0;
  int64_t reasoning = 0;
  double cost = 0;
  bool cost_reported = false;
  int64_t cache_write = 0;
  int64_t web_searches = 0;

  int64_t GeneratedTokens() const {
    return SaturatingNonnegativeAdd(output, reasoning);
  }

  // `input` excludes the cached part, so the two together are the whole
  // prompt. Zero when nothing has been counted yet.
  int64_t CacheHitPercent() const {
    int64_t fresh = Nonnegative(input);
    int64_t cached = Nonnegative(cache_read);
    long double prompt =
        static_cast<long double>(fresh) + static_cast<long double>(cached);
    if (prompt <= 0) return 0;
    long double percent = 100.0L * static_cast<long double>(cached) / prompt;
    return static_cast<int64_t>(std::clamp(percent, 0.0L, 100.0L));
  }

  void Merge(const Usage& other) {
    input = SaturatingNonnegativeAdd(input, other.input);
    output = SaturatingNonnegativeAdd(output, other.output);
    cache_read = SaturatingNonnegativeAdd(cache_read, other.cache_read);
    cache_write = SaturatingNonnegativeAdd(cache_write, other.cache_write);
    reasoning = SaturatingNonnegativeAdd(reasoning, other.reasoning);
    web_searches = SaturatingNonnegativeAdd(web_searches, other.web_searches);
    MergeCost(other.cost, other.cost_reported);
  }

  // OpenAI convention: input excludes cached tokens, output excludes reasoning.
  void Add(const json& value) {
    if (!value.is_object()) return;
    Normalize();
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

    int64_t input_tokens = Nonnegative(
        first({{nullptr, "prompt_tokens"}, {nullptr, "input_tokens"}}));
    int64_t output_tokens = Nonnegative(
        first({{nullptr, "completion_tokens"}, {nullptr, "output_tokens"}}));
    int64_t reason =
        Nonnegative(first({{"completion_tokens_details", "reasoning_tokens"},
                           {"output_tokens_details", "reasoning_tokens"}}));
    // Chat Completions and Responses report cached tokens *inside* the prompt
    // total, so they must be subtracted out. Anthropic-style usage reports
    // them beside an input count that already excludes them — subtracting
    // there would under-report the fresh tokens.
    int64_t nested_cache =
        Nonnegative(first({{"prompt_tokens_details", "cached_tokens"},
                           {"input_tokens_details", "cached_tokens"}}));
    int64_t cache = nested_cache
                        ? nested_cache
                        : Nonnegative(JsonValue(
                              value, "cache_read_input_tokens", int64_t{0}));
    int64_t cache_write_tokens =
        Nonnegative(first({{"prompt_tokens_details", "cache_write_tokens"},
                           {"prompt_tokens_details", "cache_creation_tokens"},
                           {"cache_details", "cache_write_tokens"},
                           {nullptr, "cache_write_tokens"},
                           {nullptr, "cache_creation_input_tokens"}}));
    // Compatibility providers occasionally report detail counts larger than
    // their parent totals. Never surface impossible negative token counts.
    input = SaturatingNonnegativeAdd(
        input,
        nested_cache ? Nonnegative(input_tokens - nested_cache) : input_tokens);
    output =
        SaturatingNonnegativeAdd(output, Nonnegative(output_tokens - reason));
    cache_read = SaturatingNonnegativeAdd(cache_read, cache);
    cache_write = SaturatingNonnegativeAdd(cache_write, cache_write_tokens);
    reasoning = SaturatingNonnegativeAdd(reasoning, reason);
    if (value.contains("cost") && value["cost"].is_number()) {
      MergeCost(value["cost"].get<double>(), true);
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
      web_searches = SaturatingNonnegativeAdd(
          web_searches,
          JsonValue(*server_tools, "web_search_requests", int64_t{0}));
    }
  }

 private:
  void Normalize() {
    input = Nonnegative(input);
    output = Nonnegative(output);
    cache_read = Nonnegative(cache_read);
    cache_write = Nonnegative(cache_write);
    reasoning = Nonnegative(reasoning);
    web_searches = Nonnegative(web_searches);
    NormalizeCost();
  }

  void NormalizeCost() {
    if (!std::isfinite(cost) || cost < 0) {
      cost = 0;
      cost_reported = false;
    }
  }

  void MergeCost(double addition, bool reported) {
    NormalizeCost();
    if (!std::isfinite(addition) || addition < 0) return;
    constexpr double kMax = std::numeric_limits<double>::max();
    cost = cost > kMax - addition ? kMax : cost + addition;
    cost_reported = cost_reported || reported;
  }
};

inline json UsageJson(const Usage& usage) {
  Usage normalized;
  normalized.Merge(usage);
  return {{"input", normalized.input},
          {"output", normalized.output},
          {"cache_read", normalized.cache_read},
          {"cache_write", normalized.cache_write},
          {"reasoning", normalized.reasoning},
          {"cost", normalized.cost},
          {"cost_reported", normalized.cost_reported},
          {"web_searches", normalized.web_searches}};
}

inline Usage UsageFromJson(const json& value) {
  Usage parsed;
  if (!value.is_object()) return parsed;
  parsed.input = JsonValue(value, "input", int64_t{0});
  parsed.output = JsonValue(value, "output", int64_t{0});
  parsed.cache_read = JsonValue(value, "cache_read", int64_t{0});
  parsed.cache_write = JsonValue(value, "cache_write", int64_t{0});
  parsed.reasoning = JsonValue(value, "reasoning", int64_t{0});
  parsed.cost = JsonValue(value, "cost", 0.0);
  parsed.cost_reported = JsonValue(value, "cost_reported", parsed.cost != 0);
  parsed.web_searches = JsonValue(value, "web_searches", int64_t{0});
  Usage normalized;
  normalized.Merge(parsed);
  return normalized;
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
