// Copyright 2026 Timon Gentzsch

#include "include/core/usage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

#include "include/core/checked.h"
#include "include/core/json.h"

namespace uagent {

void Usage::Add(const json& value) {
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
      const json* scope =
          alias.parent ? JsonObject(value, alias.parent) : &value;
      int64_t found = scope ? JsonValue(*scope, alias.field, int64_t{0}) : 0;
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
                      : Nonnegative(JsonValue(value, "cache_read_input_tokens",
                                              int64_t{0}));
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
  const json* server_tools = JsonObject(value, "server_tool_use_details");
  if (!server_tools) server_tools = JsonObject(value, "server_tool_use");
  if (server_tools) {
    web_searches = SaturatingNonnegativeAdd(
        web_searches,
        JsonValue(*server_tools, "web_search_requests", int64_t{0}));
  }
}

json UsageJson(const Usage& usage) {
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

Usage UsageFromJson(const json& value) {
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

bool HasUsage(const Usage& usage) {
  return usage.input || usage.output || usage.cache_read || usage.cache_write ||
         usage.reasoning || usage.web_searches || usage.cost_reported;
}

int64_t NonnegativeJsonInteger(const json& value) {
  if (value.is_number_unsigned()) {
    const uint64_t parsed = value.get<uint64_t>();
    const uint64_t maximum =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return parsed > maximum ? std::numeric_limits<int64_t>::max()
                            : static_cast<int64_t>(parsed);
  }
  return value.is_number_integer() ? Nonnegative(value.get<int64_t>()) : 0;
}

int64_t NumericStatistic(const json& object, std::string_view key) {
  if (!object.is_object()) return 0;
  const auto found = object.find(key);
  return found == object.end() ? 0 : NonnegativeJsonInteger(*found);
}

void MergeNumericStatistics(json& total, const json& delta) {
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

json PrefixNumericStatistics(const json& statistics, std::string_view prefix) {
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

json FlattenNumericStatistics(const json& statistics, std::string_view prefix) {
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

Usage UsageDifference(const Usage& current, const Usage& prior) {
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

json RouteUsageJson(const RouteUsage& routes) {
  json out = json::object();
  for (const auto& [route, usage] : routes) out[route] = UsageJson(usage);
  return out;
}

RouteUsage RouteUsageFromJson(const json& value) {
  RouteUsage routes;
  if (!value.is_object()) return routes;
  for (const auto& [route, usage] : value.items()) {
    routes[route] = UsageFromJson(usage);
  }
  return routes;
}

}  // namespace uagent
