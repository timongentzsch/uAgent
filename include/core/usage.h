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
  void Add(const json& value);

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

json UsageJson(const Usage& usage);

Usage UsageFromJson(const json& value);

using RouteUsage = std::map<std::string, Usage>;

bool HasUsage(const Usage& usage);

// Statistics are additive nonnegative counters and durations. These helpers
// are shared by parent and child accounting so nested
// work cannot gain a different merge or delta rule at each process boundary.
int64_t NonnegativeJsonInteger(const json& value);

int64_t NumericStatistic(const json& object, std::string_view key);

void MergeNumericStatistics(json& total, const json& delta);

json PrefixNumericStatistics(const json& statistics, std::string_view prefix);

json FlattenNumericStatistics(const json& statistics, std::string_view prefix);

// What a running total gained since `prior`, an earlier reading of it.
Usage UsageDifference(const Usage& current, const Usage& prior);

struct AccumulatedUsage {
  Usage unassigned;
  RouteUsage routes;
  std::map<int64_t, Usage> turns;
  std::map<int64_t, json> turn_statistics;
};

json RouteUsageJson(const RouteUsage& routes);

RouteUsage RouteUsageFromJson(const json& value);

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
