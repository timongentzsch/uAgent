// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_RETRY_H_
#define UAGENT_INCLUDE_API_RETRY_H_

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "include/api/types.h"
#include "include/core/strings.h"

namespace uagent {

inline constexpr int kChatAttempts = 3;
// Non-streaming side requests reuse the conversation's attempt budget.
inline constexpr int kSideAttempts = 3;

inline bool RetryableHttpStatus(int64_t status) {
  return status == 408 || status == 409 || status == 429 || status >= 500;
}

inline constexpr std::array<std::string_view, 20> kTransientErrors = {
    "server_error",
    "api_error",
    "overloaded_error",
    "server_is_overloaded",
    "slow_down",
    "service_unavailable",
    "service_unavailable_error",
    "provider_unavailable",
    "provider_overloaded",
    "provider_returned_error",
    "model_at_capacity",
    "internal_error",
    "internal_server_error",
    "rate_limit_error",
    "rate_limit_exceeded",
    "too_many_requests",
    "resource_exhausted",
    "timeout",
    "timeout_error",
    "request_timeout",
};

inline constexpr std::array<std::string_view, 7> kContextErrors = {
    "context_length_exceeded",
    "context_window_exceeded",
    "model_context_window_exceeded",
    "context_overflow",
    "prompt_too_long",
    "request_too_large",
    "input_too_long",
};

inline bool ErrorNameIn(std::string_view value, const auto& names) {
  return std::find(names.begin(), names.end(), value) != names.end();
}

inline bool RetryableRemoteError(std::string_view type, std::string_view code) {
  return ErrorNameIn(type, kTransientErrors) ||
         ErrorNameIn(code, kTransientErrors);
}

inline bool RemoteMessageCode(std::string_view message, std::string_view code) {
  for (size_t at = message.find(code); at != std::string_view::npos;
       at = message.find(code, at + 1)) {
    size_t end = at + code.size();
    if (at > 0 && end < message.size() &&
        ((message[at - 1] == '\'' && message[end] == '\'') ||
         (message[at - 1] == '"' && message[end] == '"'))) {
      return true;
    }
  }
  return false;
}

// Condensed port of opencode's isContextOverflow() matcher
// (packages/llm/src/provider-error.ts): one distinctive substring per
// provider message shape, with opencode's throttling exclusions applied
// first. HTTP 413 is already mapped in client.cc.
inline constexpr std::array<std::string_view, 25> kContextOverflowPhrases = {
    "prompt is too long",
    "request_too_large",
    "input is too long",
    "exceeds the context window",
    "maximum context length",
    "input token count",
    "max tokens allowed",
    "maximum prompt length",
    "reduce the length of the messages",
    "maximum allowed input length",
    "is longer than the model",
    "exceeds the limit of",
    "exceeds the available context size",
    "greater than the context length",
    "context window exceeds",
    "exceeded model token limit",
    "context_length_exceeded",
    "context length exceeded",
    "request entity too large",
    "context length is only",
    "exceeds the context length",
    "configured context size",
    "model_context_window_exceeded",
    "too many tokens",
    "token limit exceeded",
};

inline constexpr std::array<std::string_view, 4> kContextOverflowExclusions = {
    "throttling error",
    "service unavailable",
    "rate limit",
    "too many requests",
};

inline bool SemanticContextOverflow(std::string_view message) {
  std::string text(message);
  for (std::string_view phrase : kContextOverflowExclusions) {
    if (ContainsCaseInsensitive(text, std::string(phrase))) return false;
  }
  for (std::string_view phrase : kContextOverflowPhrases) {
    if (ContainsCaseInsensitive(text, std::string(phrase))) return true;
  }
  return false;
}

inline bool RetryableRemoteMessage(std::string_view message) {
  return std::any_of(
      kTransientErrors.begin(), kTransientErrors.end(),
      [&](std::string_view code) { return RemoteMessageCode(message, code); });
}

inline const char* RemoteErrorKindName(RemoteErrorKind kind) {
  switch (kind) {
    case RemoteErrorKind::kNone:
      return "none";
    case RemoteErrorKind::kTransient:
      return "transient";
    case RemoteErrorKind::kContextLengthExceeded:
      return "context_length_exceeded";
  }
  return "none";
}

inline bool ContextLengthError(std::string_view type, std::string_view code) {
  return ErrorNameIn(type, kContextErrors) || ErrorNameIn(code, kContextErrors);
}

inline RemoteErrorKind ClassifyRemoteError(std::string_view type,
                                           std::string_view code) {
  if (ContextLengthError(type, code)) {
    return RemoteErrorKind::kContextLengthExceeded;
  }
  return RetryableRemoteError(type, code) ? RemoteErrorKind::kTransient
                                          : RemoteErrorKind::kNone;
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

// Record a provider error's type/code on the result and say whether it is
// worth another attempt. Used for both HTTP error bodies and stream frames.
inline bool ApplyRemoteError(const json& error, ChatResult& result) {
  result.remote_error_type = RemoteErrorType(error);
  result.remote_error_code = RemoteErrorCode(error);
  std::string message = error.is_string()
                            ? error.get<std::string>()
                            : JsonValue(error, "message", std::string());
  result.remote_error_kind =
      ClassifyRemoteError(result.remote_error_type, result.remote_error_code);
  if (result.remote_error_kind == RemoteErrorKind::kNone &&
      (std::any_of(kContextErrors.begin(), kContextErrors.end(),
                   [&](std::string_view code) {
                     return RemoteMessageCode(message, code);
                   }) ||
       SemanticContextOverflow(message))) {
    result.remote_error_kind = RemoteErrorKind::kContextLengthExceeded;
  }
  return result.remote_error_kind == RemoteErrorKind::kTransient ||
         RetryableRemoteMessage(message);
}

inline bool SafeContextRecovery(const ChatResult& result) {
  return result.remote_error_kind == RemoteErrorKind::kContextLengthExceeded &&
         !result.interrupted && !result.semantic_progress &&
         result.content.empty() && result.reasoning.empty() &&
         result.reasoning_details.empty() && result.tool_calls.empty() &&
         result.annotations.empty() &&
         (result.usage.is_null() || result.usage.empty());
}

// A stream that failed before producing anything usable. Providers must
// inject mid-stream failures in-band once HTTP 200 is committed, and those
// frames carry provider-specific types no classifier can enumerate; treating a
// barren one as retryable costs a replay of a request that produced no visible
// answer, no executable call and no billed usage, where the alternative is
// discarding a whole turn's work.
inline bool BarrenStreamError(const ChatResult& result) {
  return result.content.empty() && result.tool_calls.empty() &&
         result.annotations.empty() && result.usage.is_null();
}

inline bool SafeToRetry(const ChatResult& result) {
  // Reasoning-only progress has no external side effect and is discarded with
  // a failed response. Never replay visible answer text or a completed call.
  return result.retryable &&
         result.remote_error_kind != RemoteErrorKind::kContextLengthExceeded &&
         !result.interrupted && result.content.empty() &&
         result.tool_calls.empty() && result.annotations.empty() &&
         result.usage.is_null() &&
         (!result.semantic_progress || !result.reasoning.empty());
}

// Side requests (web search, and any other non-streaming JSON call) get the
// same bounded recovery as the conversation, minus the replay concerns: a
// JSON call either landed or it did not, so there is no partial output to
// protect. A transport failure leaves no status behind; an aborted deadline is
// the caller's business, not this predicate's.
inline bool SafeToRetry(const JsonResponse& response) {
  if (response.error.empty()) return false;
  if (response.http_status == 0) return true;  // connection reset or timeout
  return RetryableHttpStatus(response.http_status) ||
         RetryableRemoteMessage(response.error);
}

// The clock is the only entropy this needs: two attempts never coincide, and
// nothing here has to be reproducible.
inline uint64_t JitterSeed() {
  return static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
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
