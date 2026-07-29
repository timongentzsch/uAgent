// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_USAGE_H_
#define UAGENT_INCLUDE_CORE_USAGE_H_

#include <cstdint>
#include <mutex>

#include "include/core/json.h"

namespace uagent {

using nlohmann::json;

struct Usage {
  int64_t input = 0;
  int64_t output = 0;
  int64_t cache_read = 0;
  int64_t reasoning = 0;
  double cost = 0;
  int64_t cache_write = 0;
  int64_t web_searches = 0;

  void Merge(const Usage& other) {
    input += other.input;
    output += other.output;
    cache_read += other.cache_read;
    reasoning += other.reasoning;
    cost += other.cost;
    cache_write += other.cache_write;
    web_searches += other.web_searches;
  }

  // OpenAI convention: input excludes cached tokens, output excludes reasoning.
  void Add(const json& value) {
    if (!value.is_object()) return;
    auto detail = [&](const char* key, const char* field) {
      return value.contains(key) && value[key].is_object()
                 ? JsonInt(value[key], field)
                 : int64_t{0};
    };
    int64_t cache = detail("prompt_tokens_details", "cached_tokens");
    int64_t cache_write_tokens =
        detail("prompt_tokens_details", "cache_write_tokens");
    if (!cache_write_tokens) {
      cache_write_tokens = detail("cache_details", "cache_write_tokens");
    }
    if (!cache_write_tokens) {
      cache_write_tokens = JsonInt(value, "cache_write_tokens");
    }
    int64_t reason = detail("completion_tokens_details", "reasoning_tokens");
    input += JsonInt(value, "prompt_tokens") - cache;
    output += JsonInt(value, "completion_tokens") - reason;
    cache_read += cache;
    cache_write += cache_write_tokens;
    reasoning += reason;
    cost += JsonNumber(value, "cost");
    if (value.contains("server_tool_use") &&
        value["server_tool_use"].is_object()) {
      web_searches += JsonInt(value["server_tool_use"], "web_search_requests");
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
  usage.web_searches = JsonValue(value, "web_searches", int64_t{0});
  return usage;
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

  Usage Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Usage usage = usage_;
    usage_ = {};
    return usage;
  }

 private:
  std::mutex mutex_;
  Usage usage_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_USAGE_H_
