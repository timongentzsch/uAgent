// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_RETRY_H_
#define UAGENT_INCLUDE_API_RETRY_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "include/api/types.h"

namespace uagent {

inline constexpr int kChatAttempts = 3;

inline bool RetryableHttpStatus(int64_t status) {
  return status == 408 || status == 409 || status == 429 || status >= 500;
}

inline bool RetryableRemoteError(std::string_view type, std::string_view code) {
  return type == "server_error" || type == "service_unavailable_error" ||
         type == "rate_limit_error" || type == "timeout_error" ||
         type == "timeout" || type == "provider_unavailable" ||
         type == "provider_overloaded" || code == "server_error" ||
         code == "server_is_overloaded" || code == "model_at_capacity" ||
         code == "rate_limit_exceeded" || code == "request_timeout";
}

inline bool RetryableRemoteMessage(std::string_view message) {
  auto has = [&](std::string_view code) {
    return message.find("'" + std::string(code) + "'") !=
               std::string_view::npos ||
           message.find("\"" + std::string(code) + "\"") !=
               std::string_view::npos;
  };
  return has("server_is_overloaded") || has("model_at_capacity") ||
         has("rate_limit_exceeded") || has("request_timeout");
}

// OpenAI-style endpoints put the type/code directly on `error`. OpenRouter's
// stable cross-provider type lives in `error.metadata.error_type`.
inline std::string RemoteErrorType(const json& error) {
  if (error.contains("metadata") && error["metadata"].is_object()) {
    std::string type = JsonValue(error["metadata"], "error_type", "");
    if (!type.empty()) return type;
  }
  return JsonValue(error, "type", "");
}

inline std::string RemoteErrorCode(const json& error) {
  std::string code = JsonValue(error, "code", "");
  if (!code.empty()) return code;
  if (error.contains("metadata") && error["metadata"].is_object()) {
    return JsonValue(error["metadata"], "provider_code", "");
  }
  return "";
}

inline bool SafeToRetry(const ChatResult& result) {
  // Reasoning-only progress has no external side effect and is discarded with
  // a failed response. Never replay visible answer text or a completed call.
  return result.retryable && !result.interrupted && result.content.empty() &&
         result.tool_calls.empty() && result.annotations.empty() &&
         result.usage.is_null() &&
         (!result.semantic_progress || !result.reasoning.empty());
}

inline std::chrono::milliseconds RetryDelay(int failed_attempt,
                                            uint64_t jitter_seed = 0) {
  constexpr int64_t kInitialMs = 500;
  constexpr int64_t kMaximumMs = 8'000;
  int shift = failed_attempt > 0 ? failed_attempt - 1 : 0;
  shift = shift > 4 ? 4 : shift;
  int64_t base = std::min(kMaximumMs, kInitialMs * (int64_t{1} << shift));
  int64_t jitter_per_mille = 750 + static_cast<int64_t>(jitter_seed % 251);
  return std::chrono::milliseconds(base * jitter_per_mille / 1'000);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_RETRY_H_
