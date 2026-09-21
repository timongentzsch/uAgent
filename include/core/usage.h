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
#include <string_view>

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

  // Input, cache reads and cache writes are disjoint parts of the prompt.
  // Zero when nothing has been counted yet.
  // A percentage of two token counts: double carries far more precision than
  // the integer result needs.
  int64_t CacheHitPercent() const {
    int64_t fresh = Nonnegative(input);
    int64_t cached = Nonnegative(cache_read);
    double prompt = static_cast<double>(fresh) + static_cast<double>(cached) +
                    static_cast<double>(Nonnegative(cache_write));
    if (prompt <= 0) return 0;
    double percent = 100.0 * static_cast<double>(cached) / prompt;
    return static_cast<int64_t>(std::clamp(percent, 0.0, 100.0));
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
    int64_t nested_write =
        Nonnegative(first({{"prompt_tokens_details", "cache_write_tokens"},
                           {"prompt_tokens_details", "cache_creation_tokens"},
                           {"input_tokens_details", "cache_write_tokens"}}));
    int64_t cache_write_tokens =
        nested_write
            ? nested_write
            : Nonnegative(first({{"cache_details", "cache_write_tokens"},
                                 {nullptr, "cache_write_tokens"},
                                 {nullptr, "cache_creation_input_tokens"}}));
    // Compatibility providers occasionally report detail counts larger than
    // their parent totals. Never surface impossible negative token counts.
    input = SaturatingNonnegativeAdd(
        input,
        Nonnegative(Nonnegative(input_tokens - nested_cache) - nested_write));
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

inline bool HasUsage(const Usage& usage) {
  return usage.input || usage.output || usage.cache_read || usage.cache_write ||
         usage.reasoning || usage.web_searches || usage.cost_reported;
}

// Statistics are additive nonnegative counters and durations. These helpers
// are shared by parent, child and persistent-collaborator accounting so nested
// work cannot gain a different merge or delta rule at each process boundary.
inline int64_t NonnegativeJsonInteger(const json& value) {
  if (value.is_number_unsigned()) {
    const uint64_t parsed = value.get<uint64_t>();
    const uint64_t maximum =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return parsed > maximum ? std::numeric_limits<int64_t>::max()
                            : static_cast<int64_t>(parsed);
  }
  return value.is_number_integer() ? Nonnegative(value.get<int64_t>()) : 0;
}

inline int64_t NumericStatistic(const json& object, std::string_view key) {
  if (!object.is_object()) return 0;
  const auto found = object.find(key);
  return found == object.end() ? 0 : NonnegativeJsonInteger(*found);
}

inline void MergeNumericStatistics(json& total, const json& delta) {
  if (!total.is_object()) total = json::object();
  if (!delta.is_object()) return;
  for (const auto& [key, value] : delta.items()) {
    if (!value.is_number() || !std::isfinite(value.get<double>()) ||
        value.get<double>() < 0 || key == "complete") {
      continue;
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
      total[key] = SaturatingNonnegativeAdd(NumericStatistic(total, key),
                                            NonnegativeJsonInteger(value));
    } else {
      total[key] =
          std::min(std::numeric_limits<double>::max(),
                   JsonValue(total, key.c_str(), 0.0) + value.get<double>());
    }
  }
}

inline json PrefixNumericStatistics(const json& statistics,
                                    std::string_view prefix) {
  json result = json::object();
  if (!statistics.is_object()) return result;
  for (const auto& [key, value] : statistics.items()) {
    if (!value.is_number() || key == "complete") continue;
    const std::string target =
        key.starts_with(prefix) ? key : std::string(prefix) + key;
    MergeNumericStatistics(result, {{target, value}});
  }
  return result;
}

inline json FlattenNumericStatistics(const json& statistics,
                                     std::string_view prefix) {
  json result = json::object();
  if (!statistics.is_object()) return result;
  for (const auto& [key, value] : statistics.items()) {
    if (!value.is_number() || key == "complete") continue;
    const std::string target =
        key.starts_with(prefix) ? key.substr(prefix.size()) : key;
    MergeNumericStatistics(result, {{target, value}});
  }
  return result;
}

inline Usage UsageDifference(const Usage& current, const Usage& prior) {
  const auto difference = [](int64_t now, int64_t before) {
    now = Nonnegative(now);
    before = Nonnegative(before);
    return now > before ? now - before : int64_t{0};
  };
  Usage result;
  result.input = difference(current.input, prior.input);
  result.output = difference(current.output, prior.output);
  result.cache_read = difference(current.cache_read, prior.cache_read);
  result.cache_write = difference(current.cache_write, prior.cache_write);
  result.reasoning = difference(current.reasoning, prior.reasoning);
  result.web_searches = difference(current.web_searches, prior.web_searches);
  const double now =
      std::isfinite(current.cost) && current.cost > 0 ? current.cost : 0.0;
  const double before =
      std::isfinite(prior.cost) && prior.cost > 0 ? prior.cost : 0.0;
  result.cost = now > before ? now - before : 0.0;
  result.cost_reported = current.cost_reported;
  return result;
}

inline json NumericStatisticsDifference(const json& current,
                                        const json& prior) {
  json result = json::object();
  if (!current.is_object()) return result;
  for (const auto& [key, value] : current.items()) {
    if (key == "complete" || !value.is_number() ||
        !std::isfinite(value.get<double>()) || value.get<double>() < 0) {
      continue;
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
      const int64_t now = NonnegativeJsonInteger(value);
      const int64_t before = NumericStatistic(prior, key);
      result[key] = now > before ? now - before : int64_t{0};
    } else {
      result[key] = std::max(
          0.0, value.get<double>() - JsonValue(prior, key.c_str(), 0.0));
    }
  }
  return result;
}

struct AccumulatedUsage {
  Usage unassigned;
  RouteUsage routes;
  std::map<int64_t, Usage> turns;
  std::map<int64_t, json> turn_statistics;
};

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
    unassigned_.Add(usage);
  }

  void Add(const Usage& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    unassigned_.Merge(usage);
  }

  void Add(const std::string& route, const Usage& usage, int64_t turn = 0,
           const json& statistics = json::object()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (turn > 0) {
      turns_[turn].Merge(usage);
      MergeNumericStatistics(turn_statistics_[turn], statistics);
    } else {
      unassigned_.Merge(usage);
    }
    routes_[route].Merge(usage);
  }

  Usage Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Usage usage = unassigned_;
    for (const auto& [_, attributed] : turns_) usage.Merge(attributed);
    unassigned_ = {};
    turns_.clear();
    turn_statistics_.clear();
    return usage;
  }

  RouteUsage TakeRoutes() {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteUsage routes;
    routes.swap(routes_);
    return routes;
  }

  // Usage, route attribution, parent-turn attribution and child statistics
  // are one accounting record. Taking them under one lock prevents a producer
  // from landing between separate drains and losing its route or turn.
  AccumulatedUsage TakeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    AccumulatedUsage result;
    result.unassigned = unassigned_;
    result.routes.swap(routes_);
    result.turns.swap(turns_);
    result.turn_statistics.swap(turn_statistics_);
    unassigned_ = {};
    return result;
  }

 private:
  std::mutex mutex_;
  Usage unassigned_;
  RouteUsage routes_;
  std::map<int64_t, Usage> turns_;
  std::map<int64_t, json> turn_statistics_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_USAGE_H_
