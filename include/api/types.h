// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_TYPES_H_
#define UAGENT_INCLUDE_API_TYPES_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

enum class RemoteErrorKind : uint8_t {
  kNone,
  kTransient,
  kContextLengthExceeded,
};

// Provider-specific stop spellings collapse into one turn policy while the
// raw reason and details remain available for diagnostics.
enum class ResponseStopCause : uint8_t {
  kNone,
  kComplete,
  kLength,
  kPause,
  kPolicy,
  kInputLimit,
  kTimeLimit,
  kOther,
};

inline ResponseStopCause ClassifyResponseStop(std::string_view reason) {
  if (reason.empty()) return ResponseStopCause::kNone;
  if (reason == "stop" || reason == "completed" || reason == "end_turn" ||
      reason == "tool_calls" || reason == "function_call" ||
      reason == "tool_use" || reason == "stop_sequence") {
    return ResponseStopCause::kComplete;
  }
  if (reason == "length" || reason == "max_tokens" ||
      reason == "max_output_tokens" || reason == "model_length" ||
      reason == "model_context_window_exceeded") {
    return ResponseStopCause::kLength;
  }
  if (reason == "pause_turn") return ResponseStopCause::kPause;
  if (reason == "content_filter" || reason == "refusal" || reason == "safety") {
    return ResponseStopCause::kPolicy;
  }
  if (reason == "max_prompt_tokens" || reason == "context_length") {
    return ResponseStopCause::kInputLimit;
  }
  if (reason == "max_time_limit" || reason == "time_limit") {
    return ResponseStopCause::kTimeLimit;
  }
  return ResponseStopCause::kOther;
}

inline const char* ResponseStopCauseName(ResponseStopCause cause) {
  switch (cause) {
    case ResponseStopCause::kNone:
      return "none";
    case ResponseStopCause::kComplete:
      return "complete";
    case ResponseStopCause::kLength:
      return "length";
    case ResponseStopCause::kPause:
      return "pause";
    case ResponseStopCause::kPolicy:
      return "policy";
    case ResponseStopCause::kInputLimit:
      return "input_limit";
    case ResponseStopCause::kTimeLimit:
      return "time_limit";
    case ResponseStopCause::kOther:
      return "other";
  }
  return "other";
}

// A non-streaming JSON call: the parsed body, the HTTP status, and a
// transport- or provider-level error message when the call did not land.
struct JsonResponse {
  json body = json(json::value_t::discarded);
  int64_t http_status = 0;
  std::string error;
};

// A retrieved web resource: the bytes as received and the type the origin
// declared. Nothing in the client interprets either.
struct WebResponse {
  std::string body;
  std::string content_type;
  int64_t http_status = 0;
  bool truncated = false;  // the byte cap stopped the transfer
  std::string error;
};

struct ToolCall {
  std::string id;
  std::string name;
  std::string args;
};

struct ChatResult {
  std::string content;
  std::string reasoning;
  bool reasoning_field = false;
  // True when the route emitted the `reasoning_content` extension. Routes
  // that require continuation replay receive the same observed field.
  bool reasoning_content_field = false;
  json reasoning_details = json::array();
  bool reasoning_details_field = false;
  std::vector<ToolCall> tool_calls;
  json annotations = json::array();
  // Opaque provider output needed only when the same wire API continues. Wire
  // adapters strip this from every other provider's request.
  json replay = json::object();
  json usage;
  int64_t http_status = 0;
  std::string started_at;
  double first_token_ms = -1;
  double first_event_ms = -1;
  double duration_ms = -1;
  double request_preparation_ms = -1;
  double end_to_end_ms = -1;
  double dns_ms = -1;
  double connect_ms = -1;
  double tls_ms = -1;
  double pretransfer_ms = -1;
  double start_transfer_ms = -1;
  std::string finish_reason;
  json stop_details = json::object();
  ResponseStopCause stop_cause = ResponseStopCause::kNone;
  bool incomplete = false;
  std::string error;
  std::string remote_error_type;
  std::string remote_error_code;
  RemoteErrorKind remote_error_kind = RemoteErrorKind::kNone;
  bool interrupted = false;
  bool suppressed = false;
  bool semantic_progress = false;
  bool retryable = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_TYPES_H_
