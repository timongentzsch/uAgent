// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_TYPES_H_
#define UAGENT_INCLUDE_API_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

using nlohmann::json;

struct ToolCall {
  std::string id;
  std::string name;
  std::string args;
};

struct ChatResult {
  std::string content;
  std::string reasoning;
  // True when the provider used DeepSeek's `reasoning_content` extension.
  // Direct DeepSeek-compatible routes require that field to be echoed on
  // assistant messages while a tool turn continues.
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
  bool interrupted = false;
  bool suppressed = false;
  bool semantic_progress = false;
  bool retryable = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_TYPES_H_
