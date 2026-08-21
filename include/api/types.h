// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_TYPES_H_
#define UAGENT_INCLUDE_API_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

enum class RemoteErrorKind : uint8_t {
  kNone,
  kTransient,
  kContextLengthExceeded,
};

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
  json usage;
  int64_t http_status = 0;
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
