// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_H_
#define UAGENT_INCLUDE_API_H_
// Provider-neutral API client declaration. Wire encoders and stream decoders
// live under include/api and src/api.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "include/api/capabilities.h"
#include "include/api/types.h"
#include "include/api/wire.h"
#include "include/core/env.h"

using CURL = void;
using CURLM = void;

namespace uagent {

struct StreamCtx;

// Single-owner client. Calls are intentionally serialized so one easy handle
// can retain libcurl's connection cache between requests; Api is not reentrant
// or thread-safe.
class Api {
 public:
  std::string base_url, api_key, model, reasoning_effort;
  std::vector<std::string> supported_reasoning_efforts;
  int64_t ctx_window = 0;
  ProviderCapabilities capabilities;
  bool render_stream = true;
  double session_cost = 0;
  int64_t session_generated_tokens = 0;
  // Set once at the user-turn boundary. Every transient status row in that
  // turn uses the same anchor, matching Codex's TurnStarted/TurnCompleted
  // lifetime instead of restarting for each request or tool.
  std::chrono::steady_clock::time_point turn_started;

  explicit Api(RuntimeConfig config = RuntimeConfig::FromEnvironment());
  ~Api();
  Api(const Api&) = delete;
  Api& operator=(const Api&) = delete;

  RuntimeConfig config;

  void PreserveAssistantReasoning(json& message,
                                  const ChatResult& result) const;
  std::string RequestModel() const;
  std::string CatalogModel() const;
  bool NativeHostedTool(HostedTool tool) const;
  json BuildRequestBody(const json& messages, const json& tool_schemas,
                        const std::string& session_id = "",
                        bool* web_available = nullptr,
                        WireRequestCache* cache = nullptr) const;
  std::string ChatPayload(const json& messages, const json& tool_schemas,
                          const std::string& session_id = "",
                          bool* web_available = nullptr);
  ChatResult Chat(const json& messages, const json& tool_schemas,
                  int64_t timeout_s = 0, const std::string& session_id = "",
                  bool render_output = true, size_t estimated_bytes = 0,
                  bool full_reasoning = true);
  // timeout_s bounds one attempt; attempts>1 adds the same bounded backoff
  // the conversation gets, for transport failures and transient statuses.
  JsonResponse Post(const std::string& path, const json& body,
                    int64_t timeout_s = 120, int attempts = 1);
  json Get(const std::string& path, bool abortable = false,
           int64_t timeout_s = 15);
  // A direct GET of an absolute public URL: no credentials or environment
  // proxy, and every resolved address across redirects must be public. For
  // content this client does not interpret, so the bytes come back as they
  // arrived.
  WebResponse GetUrl(const std::string& url, int64_t timeout_s, size_t cap);

 private:
  ChatResult PerformChat(const std::string& payload, bool web_available,
                         int64_t timeout_s, const std::string& session_id,
                         bool render_output, bool full_reasoning);
  bool WaitForRetry(std::chrono::milliseconds delay, bool render_output) const;
  JsonResponse Fetch(const std::string& path, const std::string* payload,
                     int64_t timeout_s, bool abortable);
  static void SetAbortable(CURL* handle, StreamCtx* context = nullptr);
  CURL* Prepare(const std::string& url);
  // 0 means unbounded, which is how both response readers spell "no cap".
  size_t ResponseCap() const {
    return config.response_bytes > 0
               ? static_cast<size_t>(config.response_bytes)
               : 0;
  }

  CURL* handle_;
  CURLM* multi_;
  WireRequestCache wire_cache_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_H_
