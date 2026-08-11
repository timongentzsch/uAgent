// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_H_
#define UAGENT_INCLUDE_API_H_
// OpenAI-compatible client declaration. Curl/SSE/rendering details live in
// src/api/client.cc and include/api/stream.h.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "include/api/types.h"
#include "include/core/env.h"

typedef void CURL;

namespace uagent {

using nlohmann::json;

struct StreamCtx;

// Single-owner client. Calls are intentionally serialized so one easy handle
// can retain libcurl's connection cache between requests; Api is not reentrant
// or thread-safe.
class Api {
 public:
  std::string base_url, api_key, model, reasoning_effort;
  int64_t ctx_window = 0;
  bool native_tools = true;
  bool include_usage = true;
  bool parallel_tools = true;
  bool openrouter_web_search = true;
  bool server_tools_authorized = false;
  bool render_stream = true;
  bool image_input = true;
  bool route_certified = false;
  bool openrouter_compatible = false;
  double session_cost = 0;
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
  json BuildChatBody(const json& messages, const json& tool_schemas,
                     const std::string& session_id = "",
                     bool* web_available = nullptr) const;
  ChatResult Chat(const json& messages, const json& tool_schemas,
                  int64_t timeout_s = 0, const std::string& session_id = "",
                  bool render_output = true);
  json Post(const std::string& path, const json& body, int64_t timeout_s = 120);
  json Get(const std::string& path, bool abortable = false);

 private:
  ChatResult PerformChat(const std::string& payload, bool web_available,
                         int64_t timeout_s, const std::string& session_id,
                         bool render_output);
  bool WaitForRetry(std::chrono::milliseconds delay, bool render_output) const;
  json Fetch(const std::string& path, const std::string* payload,
             int64_t timeout_s, bool abortable);
  static void SetAbortable(CURL* handle, StreamCtx* context = nullptr);
  CURL* Prepare(const std::string& url);

  CURL* handle_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_H_
