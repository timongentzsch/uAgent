// Copyright 2026 Timon Gentzsch

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "include/api.h"
#include "include/api/retry.h"
#include "include/api/stream.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"

namespace uagent {

namespace {

class CurlHeaders {
 public:
  CurlHeaders() = default;
  ~CurlHeaders() { curl_slist_free_all(list_); }
  CurlHeaders(const CurlHeaders&) = delete;
  CurlHeaders& operator=(const CurlHeaders&) = delete;

  bool Add(const std::string& header) {
    curl_slist* appended = curl_slist_append(list_, header.c_str());
    if (!appended) return false;
    list_ = appended;
    return true;
  }

  curl_slist* Get() const { return list_; }

 private:
  curl_slist* list_ = nullptr;
};

// libcurl's CURLOPT_TIMEOUT ABI requires long rather than a fixed-width type.
long CurlTimeout(int64_t seconds) {  // NOLINT: libcurl ABI
  return static_cast<long>(          // NOLINT: libcurl ABI
      std::clamp(
          seconds, int64_t{0},
          static_cast<int64_t>(std::numeric_limits<long>::max())));  // NOLINT
}

double CurlTimingMs(CURL* handle, CURLINFO info) {
  double seconds = 0;
  return curl_easy_getinfo(handle, info, &seconds) == CURLE_OK
             ? seconds * 1000.0
             : -1;
}

void CollectCurlTimings(CURL* handle, ChatResult& result) {
  result.dns_ms = CurlTimingMs(handle, CURLINFO_NAMELOOKUP_TIME);
  result.connect_ms = CurlTimingMs(handle, CURLINFO_CONNECT_TIME);
  result.tls_ms = CurlTimingMs(handle, CURLINFO_APPCONNECT_TIME);
  result.pretransfer_ms = CurlTimingMs(handle, CURLINFO_PRETRANSFER_TIME);
  result.start_transfer_ms = CurlTimingMs(handle, CURLINFO_STARTTRANSFER_TIME);
}

bool StreamDeadlineExpired(StreamCtx* context) {
  if (!context) return false;
  auto now = std::chrono::steady_clock::now();
  if (context->res->first_event_ms < 0 && context->first_event_timeout_s > 0 &&
      now >= context->started +
                 std::chrono::seconds(context->first_event_timeout_s)) {
    context->timeout_reason = "model produced no event within " +
                              std::to_string(context->first_event_timeout_s) +
                              "s";
    return true;
  }
  if (context->idle_timeout_s > 0 &&
      now >=
          context->last_byte + std::chrono::seconds(context->idle_timeout_s)) {
    context->timeout_reason = "model stream was idle for " +
                              std::to_string(context->idle_timeout_s) + "s";
    return true;
  }
  return false;
}

int CurlPollTimeout(CURLM* multi, StreamCtx* context) {
  long curl_timeout = -1;  // NOLINT: libcurl ABI
  (void)curl_multi_timeout(multi, &curl_timeout);
  int64_t timeout = curl_timeout >= 0 ? curl_timeout : 10000;
  if (context) {
    auto deadline = std::chrono::steady_clock::time_point::max();
    if (context->res->first_event_ms < 0 &&
        context->first_event_timeout_s > 0) {
      deadline = context->started +
                 std::chrono::seconds(context->first_event_timeout_s);
    }
    if (context->idle_timeout_s > 0) {
      deadline =
          std::min(deadline, context->last_byte +
                                 std::chrono::seconds(context->idle_timeout_s));
    }
    if (deadline != std::chrono::steady_clock::time_point::max()) {
      timeout = std::min<int64_t>(timeout, PollTimeoutMs(deadline));
    }
  }
  return static_cast<int>(
      std::clamp<int64_t>(timeout, 0, std::numeric_limits<int>::max()));
}

CURLcode PerformWithAbortWake(CURLM* multi, CURL* easy,
                              StreamCtx* context = nullptr) {
  if (!multi) return CURLE_FAILED_INIT;
  CURLcode result = CURLE_FAILED_INIT;
  if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
    return result;
  }

  int running = 0;
  CURLMcode multi_result = curl_multi_perform(multi, &running);
  while (multi_result == CURLM_OK && running > 0 && !AbortRequested() &&
         !StreamDeadlineExpired(context)) {
    curl_waitfd wake = {AbortWakeFd(), CURL_WAIT_POLLIN, 0};
    int descriptors = 0;
#if LIBCURL_VERSION_NUM >= 0x074200
    multi_result = curl_multi_poll(
        multi, &wake, 1, CurlPollTimeout(multi, context), &descriptors);
#else
    multi_result = curl_multi_wait(
        multi, &wake, 1, CurlPollTimeout(multi, context), &descriptors);
#endif
    if ((wake.revents & CURL_WAIT_POLLIN) && !AbortRequested()) {
      NormalizeAbortWake();
    }
    if (multi_result == CURLM_OK && !AbortRequested()) {
      multi_result = curl_multi_perform(multi, &running);
    }
  }

  if (AbortRequested() || (running > 0 && StreamDeadlineExpired(context))) {
    result = CURLE_ABORTED_BY_CALLBACK;
  } else if (multi_result == CURLM_OK) {
    int remaining = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &remaining)) {
      if (message->msg == CURLMSG_DONE) result = message->data.result;
    }
  } else {
    result = CURLE_RECV_ERROR;
  }
  curl_multi_remove_handle(multi, easy);
  return result;
}

}  // namespace

Api::Api(RuntimeConfig config)
    : config(std::move(config)),
      handle_(curl_easy_init()),
      multi_(curl_multi_init()) {}

Api::~Api() {
  if (multi_) curl_multi_cleanup(multi_);
  if (handle_) curl_easy_cleanup(handle_);
}

void Api::PreserveAssistantReasoning(json& message,
                                     const ChatResult& result) const {
  if (!result.reasoning_details.empty()) {
    message["reasoning_details"] = result.reasoning_details;
  } else if (capabilities.reasoning_replay_text && result.reasoning_field &&
             !result.reasoning.empty()) {
    message["reasoning"] = result.reasoning;
  } else if (result.reasoning_content_field && !result.reasoning.empty()) {
    message["reasoning_content"] = result.reasoning;
  } else if (result.reasoning_details_field) {
    // Preserve an explicitly emitted empty array: absent and empty can carry
    // different continuation semantics on OpenAI-compatible routes.
    message["reasoning_details"] = result.reasoning_details;
  }
}

std::string Api::CatalogModel() const {
  if (!capabilities.model_variants) return model;
  std::string base = model;
  bool stripped = true;
  while (stripped) {
    stripped = false;
    for (std::string_view variant : kOpenRouterVariants) {
      std::string suffix = ":" + std::string(variant);
      if (!base.ends_with(suffix)) continue;
      base.resize(base.size() - suffix.size());
      stripped = true;
      break;
    }
  }
  return base;
}

std::string Api::RequestModel() const {
  std::string base = CatalogModel();
  if (!capabilities.model_variants || config.openrouter_variant.empty()) {
    return base;
  }
  return base + ":" + config.openrouter_variant;
}

json Api::BuildChatBody(const json& messages, const json& tool_schemas,
                        const std::string& session_id,
                        bool* web_available) const {
  if (web_available) *web_available = false;
  json body = {
      {"model", RequestModel()}, {"messages", messages}, {"stream", true}};
  if (capabilities.native_tools && !tool_schemas.empty()) {
    body["tools"] = tool_schemas;
    for (const json& tool : tool_schemas) {
      if (web_available && tool.is_object() && tool.contains("function") &&
          tool["function"].is_object() &&
          JsonValue(tool["function"], "name", "") == "web_search") {
        *web_available = true;
      }
    }
    if (capabilities.parallel_tools) body["parallel_tool_calls"] = true;
  }
  if (capabilities.stream_usage_option) {
    body["stream_options"] = {{"include_usage", true}};
  }
  int64_t max_tokens = MaxOutputTokens();
  if (max_tokens > 0) {
    body[capabilities.max_completion_tokens ? "max_completion_tokens"
                                            : "max_tokens"] = max_tokens;
  }
  if (!reasoning_effort.empty()) {
    if (capabilities.reasoning_object) {
      body["reasoning"] = {{"effort", reasoning_effort}};
    } else {
      body["reasoning_effort"] = reasoning_effort;
    }
  }
  if (capabilities.session_passthrough && !session_id.empty()) {
    body["session_id"] = session_id;
  }
  if (capabilities.provider_routing && !config.openrouter_provider.empty()) {
    body["provider"] = {{"order", json::array({config.openrouter_provider})},
                        {"allow_fallbacks", config.openrouter_fallbacks}};
  }
  return body;
}

ChatResult Api::Chat(const json& messages, const json& tool_schemas,
                     int64_t timeout_s, const std::string& session_id,
                     bool render_output, size_t estimated_bytes,
                     bool full_reasoning) {
  ChatResult res;
  auto overall_started = std::chrono::steady_clock::now();
  size_t estimated = estimated_bytes;
  if (estimated == 0) {
    estimated = JsonEstimatedBytes(messages) + JsonEstimatedBytes(tool_schemas);
  }
  if (config.request_bytes > 0 &&
      estimated > static_cast<size_t>(config.request_bytes)) {
    res.error =
        "request exceeds " + std::to_string(config.request_bytes) + " bytes";
    res.request_preparation_ms = ElapsedMs(overall_started);
    res.end_to_end_ms = res.request_preparation_ms;
    return res;
  }
  bool web_available = false;
  json body = BuildChatBody(messages, tool_schemas, session_id, &web_available);
  std::string payload = JsonDump(body);
  if (config.request_bytes > 0 &&
      payload.size() > static_cast<size_t>(config.request_bytes)) {
    res.error = "serialized request exceeds " +
                std::to_string(config.request_bytes) + " bytes";
    res.request_preparation_ms = ElapsedMs(overall_started);
    res.end_to_end_ms = res.request_preparation_ms;
    return res;
  }
  double preparation_ms = ElapsedMs(overall_started);

  // timeout_s is the caller's total budget (normally the remaining turn).
  // UAGENT_REQUEST_TIMEOUT caps one transport attempt. Keeping those clocks
  // separate leaves room for retry backoff after a timed-out attempt.
  int64_t attempt_limit = config.request_timeout_s;
  int64_t request_timeout = timeout_s;
  if (request_timeout <= 0) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    request_timeout = attempt_limit <= 0 ? 0
                      : attempt_limit > kMax / kChatAttempts
                          ? kMax
                          : attempt_limit * kChatAttempts;
  }
  auto started = std::chrono::steady_clock::now();
  auto deadline = request_timeout > 0
                      ? DeadlineAfter(started, request_timeout)
                      : std::chrono::steady_clock::time_point::max();
  for (int attempt = 1; attempt <= kChatAttempts; ++attempt) {
    int64_t attempt_timeout = attempt_limit;
    if (request_timeout > 0) {
      auto remaining = std::chrono::ceil<std::chrono::seconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        res.error = "request deadline exhausted before retry";
        res.duration_ms = ElapsedMs(started);
        res.request_preparation_ms = preparation_ms;
        res.end_to_end_ms = ElapsedMs(overall_started);
        return res;
      }
      attempt_timeout = attempt_limit > 0
                            ? std::min(attempt_limit, remaining.count())
                            : remaining.count();
    }
    auto attempt_started = std::chrono::steady_clock::now();
    res = PerformChat(payload, web_available, attempt_timeout, session_id,
                      render_output, full_reasoning);
    res.request_preparation_ms = preparation_ms;
    res.end_to_end_ms = ElapsedMs(overall_started);
    if (res.first_event_ms >= 0) {
      res.first_event_ms +=
          std::chrono::duration<double, std::milli>(attempt_started - started)
              .count();
    }
    res.duration_ms = ElapsedMs(started);
    res.end_to_end_ms = ElapsedMs(overall_started);
    if (attempt == kChatAttempts || !SafeToRetry(res)) return res;

    uint64_t jitter_seed = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::chrono::milliseconds delay = RetryDelay(attempt, jitter_seed);
    if (request_timeout > 0 &&
        std::chrono::steady_clock::now() + delay >= deadline) {
      res.end_to_end_ms = ElapsedMs(overall_started);
      return res;
    }
    DebugLog("api_retry", {{"attempt", attempt},
                           {"max_attempts", kChatAttempts},
                           {"delay_ms", delay.count()},
                           {"http_status", res.http_status},
                           {"remote_error_type", res.remote_error_type},
                           {"remote_error_code", res.remote_error_code}});
    if (render_stream && render_output) {
      std::string reason = res.error.starts_with("model ")
                               ? res.error
                               : "transient provider failure";
      printf("%s· %s — retry %d/%d in %.1fs%s\n", DIM(),
             TerminalSafe(reason).c_str(), attempt, kChatAttempts - 1,
             delay.count() / 1000.0, RST());
    }
    if (!WaitForRetry(delay, render_output)) {
      res.error.clear();
      res.interrupted = true;
      res.duration_ms = ElapsedMs(started);
      res.end_to_end_ms = ElapsedMs(overall_started);
      return res;
    }
  }
  return res;
}

JsonResponse Api::Post(const std::string& path, const json& body,
                       int64_t timeout_s) {
  std::string payload = JsonDump(body);
  return Fetch(path, &payload, timeout_s, /*abortable=*/true);
}

json Api::Get(const std::string& path, bool abortable) {
  return Fetch(path, nullptr, 15, abortable).body;
}

ChatResult Api::PerformChat(const std::string& payload, bool web_available,
                            int64_t timeout_s, const std::string& session_id,
                            bool render_output, bool full_reasoning) {
  ChatResult res;
  CURL* h = Prepare(base_url + "/chat/completions");
  if (!h) {
    res.error = "curl init failed";
    return res;
  }
  StreamCtx ctx;
  ctx.handle = h;
  ctx.res = &res;
  ctx.started = std::chrono::steady_clock::now();
  ctx.last_byte = ctx.started;
  ctx.first_event_timeout_s = config.first_event_timeout_s;
  ctx.idle_timeout_s = config.stream_idle_timeout_s;
  ctx.response_cap = ResponseCap();
  ctx.sse = SseParser(ctx.response_cap);
  CurlHeaders headers;
  bool headers_ok = headers.Add("Content-Type: application/json") &&
                    headers.Add("Authorization: Bearer " + api_key) &&
                    headers.Add("Accept: text/event-stream");
  if (capabilities.session_passthrough && !session_id.empty()) {
    headers_ok = headers_ok && headers.Add("X-Session-Id: " + session_id);
  }
  if (!headers_ok) {
    res.error = "failed to allocate HTTP headers";
    return res;
  }
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.Get());
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(payload.size()));
  curl_easy_setopt(
      h, CURLOPT_WRITEFUNCTION,
      +[](char* data, size_t size, size_t count, void* user) -> size_t {
        std::optional<size_t> bytes = CheckedMul(size, count);
        return bytes ? static_cast<StreamCtx*>(user)->Feed(data, *bytes) : 0;
      });
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
  SetAbortable(h, &ctx);
  if (timeout_s > 0) {
    curl_easy_setopt(h, CURLOPT_TIMEOUT, CurlTimeout(timeout_s));
  }

  std::string activity = web_available ? "working · web available" : "working";
  ResponseObservation observation(render_stream && render_output,
                                  full_reasoning, std::move(activity),
                                  turn_started);

  CURLcode rc = CURLE_OK;
  bool cancelled =
      RunCancellable([&] { rc = PerformWithAbortWake(multi_, h, &ctx); });
  CollectCurlTimings(h, res);
  if (cancelled) ClearAbort();
  ctx.Finish();
  capabilities.Observe(res);
  if (ctx.show == StreamCtx::Show::kUndecided && !res.content.empty()) {
    ctx.OutputText(res.content);
  }
  if (!ctx.timeout_reason.empty()) {
    res.error = ctx.timeout_reason;
    res.retryable = true;
    return res;
  }
  if (rc == CURLE_ABORTED_BY_CALLBACK || cancelled) {
    res.interrupted = true;
    return res;
  }
  if (!res.error.empty()) return res;
  if (rc != CURLE_OK) {
    res.error = std::string("connection error: ") + curl_easy_strerror(rc);
    res.retryable = true;
    return res;
  }
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.http_status);
  if (res.http_status >= 400) {
    json error_response = json::parse(ctx.error_body, nullptr, false);
    if (error_response.is_object() && error_response.contains("error")) {
      res.retryable = ApplyRemoteError(error_response["error"], res);
    }
    if (res.http_status == 413 &&
        res.remote_error_kind == RemoteErrorKind::kNone) {
      res.remote_error_kind = RemoteErrorKind::kContextLengthExceeded;
    }
    res.error = "HTTP " + std::to_string(res.http_status) + ": " +
                JsonErrorMessage(error_response, std::move(ctx.error_body));
    res.retryable = res.retryable || RetryableHttpStatus(res.http_status);
    return res;
  }
  if (!CollectToolCalls(ctx.calls, res)) return res;
  return res;
}

bool Api::WaitForRetry(std::chrono::milliseconds delay,
                       bool render_output) const {
  TerminalSpinner spinner(render_stream && render_output,
                          SpinnerLabel("retrying"), turn_started);
  auto deadline = std::chrono::steady_clock::now() + delay;
  bool cancelled = RunCancellable([&] {
    pollfd wake = {AbortWakeFd(), POLLIN, 0};
    for (;;) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline || AbortRequested()) break;
      int ready = poll(&wake, 1, PollTimeoutMs(deadline));
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0) break;
      if (AbortRequested()) break;
      NormalizeAbortWake();
      wake.revents = 0;
    }
  });
  if (cancelled) ClearAbort();
  return !cancelled;
}

JsonResponse Api::Fetch(const std::string& path, const std::string* payload,
                        int64_t timeout_s, bool abortable) {
  JsonResponse result;
  CURL* h = Prepare(base_url + path);
  if (!h) {
    result.error = "curl init failed";
    return result;
  }
  struct FetchBuffer {
    std::string data;
    size_t cap = 0;
    bool exceeded = false;
  } out;
  out.cap = ResponseCap();
  CurlHeaders headers;
  bool headers_ok = headers.Add("Authorization: Bearer " + api_key);
  if (payload) {
    headers_ok = headers_ok && headers.Add("Content-Type: application/json");
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload->c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(payload->size()));
  }
  if (!headers_ok) {
    result.error = "failed to allocate HTTP headers";
    return result;
  }
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.Get());
  curl_easy_setopt(
      h, CURLOPT_WRITEFUNCTION,
      +[](char* d, size_t s, size_t n, void* u) -> size_t {
        auto* out = static_cast<FetchBuffer*>(u);
        std::optional<size_t> bytes = CheckedMul(s, n);
        if (!bytes || (out->cap > 0 &&
                       AdditionExceeds(out->data.size(), *bytes, out->cap))) {
          out->exceeded = true;
          return 0;
        }
        out->data.append(d, *bytes);
        return *bytes;
      });
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
  if (abortable) SetAbortable(h);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, CurlTimeout(timeout_s));
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING,
                   "");  // 531 KB -> 63 KB on /models
  CURLcode rc =
      abortable ? PerformWithAbortWake(multi_, h) : curl_easy_perform(h);
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &result.http_status);
  if (out.exceeded) {
    result.error = "response exceeds configured byte limit";
    return result;
  }
  if (rc != CURLE_OK) {
    result.error = std::string("connection error: ") + curl_easy_strerror(rc);
    return result;
  }
  result.body = json::parse(out.data, nullptr, false);
  if (result.body.is_discarded()) {
    result.error = "invalid JSON response";
  } else if (result.http_status >= 400) {
    result.error = "HTTP " + std::to_string(result.http_status);
  }
  return result;
}

void Api::SetAbortable(CURL* h, StreamCtx* ctx) {
  curl_easy_setopt(
      h, CURLOPT_XFERINFOFUNCTION,
      +[](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        if (AbortRequested()) return 1;
        return StreamDeadlineExpired(static_cast<StreamCtx*>(user)) ? 1 : 0;
      });
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, ctx);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
}

CURL* Api::Prepare(const std::string& url) {
  if (!handle_) return nullptr;
  curl_easy_reset(handle_);  // clears options, keeps the connection cache
  curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 10L);
  // API redirects are unexpected and can cross an origin boundary.
  // Reject them rather than risk forwarding a bearer token.
  curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(handle_, CURLOPT_PROTOCOLS_STR, "http,https");
#else
  curl_easy_setopt(handle_, CURLOPT_PROTOCOLS,
                   CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
  return handle_;
}

}  // namespace uagent
