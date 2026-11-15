// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_CLIENT_H_
#define UAGENT_INCLUDE_API_CLIENT_H_

#include <sys/socket.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "include/api/capabilities.h"
#include "include/api/endpoint.h"
#include "include/api/types.h"
#include "include/api/wire.h"
#include "include/core/env.h"
#include "include/core/runtime_config.h"

using CURL = void;
using CURLM = void;

namespace uagent {

// False for loopback, private, link-local, multicast and reserved space.
bool PublicSocketAddress(const sockaddr* address, socklen_t length);

struct StreamCtx;
class HttpExchange;

// Copyable request settings; side requests snapshot this exact route.
struct ApiSettings : Route {
  ProviderCapabilities capabilities;
  RuntimeConfig config;
};

// Calls are serialized so one easy handle retains its connection cache.
class Api : public ApiSettings {
 public:
  double session_cost = 0;

  explicit Api(RuntimeConfig config = RuntimeConfig::FromEnvironment());
  explicit Api(ApiSettings settings);
  ~Api();
  Api(const Api&) = delete;
  Api& operator=(const Api&) = delete;

  bool capture_http = false;
  // Overrides for side requests. A positive cap replaces UAGENT_MAX_TOKENS;
  // forbidden tools stay in the request so the cached prefix holds; a request
  // inside a tool keeps the abort set for the turn's tool loop to read.
  int64_t max_output_tokens = 0;
  bool forbid_tool_calls = false;
  bool native_web_search = false;
  bool keep_abort = false;
  std::function<void(const json&, size_t)> observe_progress;
  json exchange_context = json::object();
  json http_exchanges = json::array();

  void PreserveAssistantReasoning(json& message,
                                  const ChatResult& result) const;
  json BuildRequestBody(const json& messages, const json& tool_schemas,
                        const std::string& session_id = "",
                        WireRequestCache* cache = nullptr) const;
  std::string ChatPayload(const json& messages, const json& tool_schemas,
                          const std::string& session_id = "");
  ChatResult Chat(const json& messages, const json& tool_schemas,
                  int64_t timeout_s = 0, const std::string& session_id = "",
                  size_t estimated_bytes = 0);
  // timeout_s bounds one attempt, deadline the whole operation with retries.
  JsonResponse Post(const std::string& path, const json& body,
                    int64_t timeout_s = 120, int attempts = 1,
                    std::chrono::steady_clock::time_point deadline =
                        std::chrono::steady_clock::time_point::max());
  JsonResponse Get(const std::string& path, bool abortable = false,
                   int64_t timeout_s = 15);
  // GET of an absolute public URL: no credentials, no environment proxy, and
  // every address resolved across redirects must be public.
  WebResponse GetUrl(const std::string& url, int64_t timeout_s, size_t cap);

 private:
  ChatResult PerformChat(const std::string& payload, int64_t timeout_s,
                         const std::string& session_id, HttpExchange* exchange,
                         json response_context);
  bool WaitForRetry(std::chrono::milliseconds delay) const;
  JsonResponse Fetch(const std::string& path, const std::string* payload,
                     int64_t timeout_s, bool abortable,
                     std::chrono::steady_clock::time_point deadline =
                         std::chrono::steady_clock::time_point::max());
  static void SetAbortable(CURL* handle, StreamCtx* context = nullptr);
  CURL* Prepare(const std::string& url);

  CURL* handle_;
  CURLM* multi_;
  WireRequestCache wire_cache_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_CLIENT_H_
