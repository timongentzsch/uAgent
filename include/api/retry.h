// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_RETRY_H_
#define UAGENT_INCLUDE_API_RETRY_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
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
         code == "server_error" || code == "server_is_overloaded" ||
         code == "model_at_capacity" || code == "rate_limit_exceeded" ||
         code == "request_timeout";
}

inline bool SafeToRetry(const ChatResult& result) {
  return result.retryable && !result.semantic_progress && !result.interrupted &&
         result.content.empty() && result.reasoning.empty() &&
         result.tool_calls.empty();
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
