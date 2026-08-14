// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "include/api.h"
#include "include/api/retry.h"
#include "include/api/stream.h"
#include "include/core/debug.h"
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

}  // namespace

Api::Api(RuntimeConfig config)
    : config(std::move(config)), handle_(curl_easy_init()) {}

Api::~Api() {
  if (handle_) curl_easy_cleanup(handle_);
}

void Api::PreserveAssistantReasoning(json& message,
                                     const ChatResult& result) const {
  if (!openrouter_compatible) return;
  if (!result.reasoning_details.empty()) {
    message["reasoning_details"] = result.reasoning_details;
  } else if (!result.reasoning.empty()) {
    message["reasoning"] = result.reasoning;
  }
}

std::string Api::RequestModel() const {
  if (!openrouter_compatible || config.openrouter_variant.empty()) return model;
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
  return base + ":" + config.openrouter_variant;
}

json Api::BuildChatBody(const json& messages, const json& tool_schemas,
                        const std::string& session_id,
                        bool* web_available) const {
  if (web_available) *web_available = false;
  json body = {
      {"model", RequestModel()}, {"messages", messages}, {"stream", true}};
  if (native_tools && !tool_schemas.empty()) {
    json request_tools = json::array();
    bool server_search = openrouter_compatible && config.web_search_server &&
                         openrouter_web_search && server_tools_authorized;
    bool search_offered = false;
    for (const json& tool : tool_schemas) {
      bool compatibility_search =
          tool.is_object() && tool.contains("function") &&
          tool["function"].is_object() &&
          JsonValue(tool["function"], "name", "") == "web_search";
      search_offered = search_offered || compatibility_search;
      if (compatibility_search && server_search) continue;
      request_tools.push_back(tool);
    }
    if (server_search && search_offered) {
      if (web_available) *web_available = true;
      json parameters = {
          {"engine", config.web_search_engine},
          {"max_results", config.web_search_max_results},
          {"max_total_results",
           config.web_search_max_results * config.web_search_max_uses},
      };
      if (!config.web_search_context_size.empty()) {
        parameters["search_context_size"] = config.web_search_context_size;
      }
      request_tools.push_back({{"type", "openrouter:web_search"},
                               {"parameters", std::move(parameters)}});
    }
    if (!request_tools.empty()) {
      body["tools"] = std::move(request_tools);
      if (parallel_tools) body["parallel_tool_calls"] = true;
    }
  }
  // OpenRouter always includes usage in the final SSE event; its
  // stream_options.include_usage switch is deprecated and adds no value.
  if (include_usage && !openrouter_compatible) {
    body["stream_options"] = {{"include_usage", true}};
  }
  int64_t max_tokens = MaxOutputTokens();
  if (max_tokens > 0) {
    body[OpenaiUrl(base_url) ? "max_completion_tokens" : "max_tokens"] =
        max_tokens;
  }
  if (!reasoning_effort.empty()) {
    if (openrouter_compatible) {
      body["reasoning"] = {{"effort", reasoning_effort}};
    } else {
      body["reasoning_effort"] = reasoning_effort;
    }
  }
  if (openrouter_compatible) {
    if (!session_id.empty()) body["session_id"] = session_id;
    if (!config.openrouter_provider.empty()) {
      body["provider"] = {{"order", json::array({config.openrouter_provider})},
                          {"allow_fallbacks", config.openrouter_fallbacks}};
    }
  }
  return body;
}

ChatResult Api::Chat(const json& messages, const json& tool_schemas,
                     int64_t timeout_s, const std::string& session_id,
                     bool render_output, size_t estimated_bytes) {
  ChatResult res;
  auto overall_started = std::chrono::steady_clock::now();
  size_t estimated = estimated_bytes;
  if (estimated == 0) {
    estimated =
        JsonEstimatedBytes(messages) + JsonEstimatedBytes(tool_schemas);
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
                      render_output);
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

json Api::Post(const std::string& path, const json& body, int64_t timeout_s) {
  std::string payload = JsonDump(body);
  return Fetch(path, &payload, timeout_s, /*abortable=*/true);
}

json Api::Get(const std::string& path, bool abortable) {
  return Fetch(path, nullptr, 15, abortable);
}

ChatResult Api::PerformChat(const std::string& payload, bool web_available,
                            int64_t timeout_s, const std::string& session_id,
                            bool render_output) {
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
  ctx.render_output = render_stream && render_output;
  ctx.response_cap = ResponseCap();
  ctx.sse = SseParser(ctx.response_cap);
  CurlHeaders headers;
  bool headers_ok = headers.Add("Content-Type: application/json") &&
                    headers.Add("Authorization: Bearer " + api_key) &&
                    headers.Add("Accept: text/event-stream");
  if (openrouter_compatible && !session_id.empty()) {
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
  TerminalSpinner spinner(ctx.render_output, SpinnerLabel(activity),
                          turn_started);
  ctx.spinner = &spinner;

  CURLcode rc = CURLE_OK;
  bool cancelled = RunCancellable([&] { rc = curl_easy_perform(h); });
  CollectCurlTimings(h, res);
  if (cancelled) ClearAbort();
  ctx.BeginOutput();
  ctx.Finish();
  if (ctx.show == StreamCtx::Show::kUndecided && !res.content.empty()) {
    ctx.OutputText(res.content);
  }
  if (ctx.render_output) {
    ctx.md.Flush();
    if (ctx.in_reasoning) printf("%s\n", RST());
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
    while (!AbortRequested() && std::chrono::steady_clock::now() < deadline) {
      auto remaining = deadline - std::chrono::steady_clock::now();
      auto quantum =
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::milliseconds(50));
      std::this_thread::sleep_for(std::min(remaining, quantum));
    }
  });
  if (cancelled) ClearAbort();
  return !cancelled;
}

json Api::Fetch(const std::string& path, const std::string* payload,
                int64_t timeout_s, bool abortable) {
  CURL* h = Prepare(base_url + path);
  if (!h) return json(json::value_t::discarded);
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
  if (!headers_ok) return json(json::value_t::discarded);
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
  CURLcode rc = curl_easy_perform(h);
  if (rc != CURLE_OK || out.exceeded) return json(json::value_t::discarded);
  return json::parse(out.data, nullptr, false);
}

void Api::SetAbortable(CURL* h, StreamCtx* ctx) {
  curl_easy_setopt(
      h, CURLOPT_XFERINFOFUNCTION,
      +[](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        if (AbortRequested()) return 1;
        auto* ctx = static_cast<StreamCtx*>(user);
        if (!ctx) return 0;
        auto now = std::chrono::steady_clock::now();
        if (ctx->res->first_event_ms < 0 && ctx->first_event_timeout_s > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - ctx->started)
                    .count() >= ctx->first_event_timeout_s) {
          ctx->timeout_reason = "model produced no event within " +
                                std::to_string(ctx->first_event_timeout_s) +
                                "s";
          return 1;
        }
        if (ctx->idle_timeout_s > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now -
                                                             ctx->last_byte)
                    .count() >= ctx->idle_timeout_s) {
          ctx->timeout_reason = "model stream was idle for " +
                                std::to_string(ctx->idle_timeout_s) + "s";
          return 1;
        }
        return 0;
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
