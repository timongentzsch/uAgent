// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_H_
#define UAGENT_INCLUDE_API_H_
// OpenAI-compatible streaming client: libcurl + SSE. Speaks to any endpoint
// (llama.cpp, vLLM, SGLang, OpenAI, Z.ai, ...) and degrades gracefully when
// a server rejects `tools` or `stream_options`.

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/api/citations.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"
#include "third_party/json.hpp"

namespace uagent {

using nlohmann::json;

struct ToolCall {
  std::string id, name, args;  // args: raw JSON string
};

struct Usage {
  int64_t input = 0, output = 0, cache_read = 0, reasoning = 0;
  double cost =
      0;  // credits (~USD), e.g. OpenRouter's `usage.cost`; 0 = not reported
  int64_t cache_write = 0;
  int64_t web_searches = 0;
  void Merge(const Usage& o) {
    input += o.input;
    output += o.output;
    cache_read += o.cache_read;
    reasoning += o.reasoning;
    cost += o.cost;
    cache_write += o.cache_write;
    web_searches += o.web_searches;
  }
  // OpenAI convention: input excludes cached tokens, output excludes reasoning.
  // A server reporting a token count as a string must not abort the turn.
  void Add(const json& u) {
    if (!u.is_object()) return;
    auto detail = [&](const char* k, const char* f) {
      return u.contains(k) && u[k].is_object() ? JsonInt(u[k], f) : int64_t{0};
    };
    int64_t cache = detail("prompt_tokens_details", "cached_tokens");
    int64_t cache_write_tokens =
        detail("prompt_tokens_details", "cache_write_tokens");
    if (!cache_write_tokens) {
      cache_write_tokens = detail("cache_details", "cache_write_tokens");
    }
    if (!cache_write_tokens) {
      cache_write_tokens = JsonInt(u, "cache_write_tokens");
    }
    int64_t reason = detail("completion_tokens_details", "reasoning_tokens");
    input += JsonInt(u, "prompt_tokens") - cache;
    output += JsonInt(u, "completion_tokens") - reason;
    cache_read += cache;
    cache_write += cache_write_tokens;
    reasoning += reason;
    cost += JsonNumber(u, "cost");
    if (u.contains("server_tool_use") && u["server_tool_use"].is_object()) {
      web_searches += JsonInt(u["server_tool_use"], "web_search_requests");
    }
  }
};

inline json UsageJson(const Usage& usage) {
  return {{"input", usage.input},
          {"output", usage.output},
          {"cache_read", usage.cache_read},
          {"cache_write", usage.cache_write},
          {"reasoning", usage.reasoning},
          {"cost", usage.cost},
          {"web_searches", usage.web_searches}};
}

// inverse of usage_json — reads back a total this or another process wrote
inline Usage UsageFromJson(const json& j) {
  Usage u;
  if (!j.is_object()) return u;
  u.input = JsonValue(j, "input", int64_t{0});
  u.output = JsonValue(j, "output", int64_t{0});
  u.cache_read = JsonValue(j, "cache_read", int64_t{0});
  u.cache_write = JsonValue(j, "cache_write", int64_t{0});
  u.reasoning = JsonValue(j, "reasoning", int64_t{0});
  u.cost = JsonValue(j, "cost", 0.0);
  u.web_searches = JsonValue(j, "web_searches", int64_t{0});
  return u;
}

// Usage from concurrent side-requests (web_search) and subagent processes.
// This is session-owned rather than process-global so independent Agent
// instances cannot accidentally bill one another.
class UsageAccumulator {
 public:
  void Add(const json& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    usage_.Add(usage);
  }
  void Add(const Usage& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    usage_.Merge(usage);
  }
  Usage Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Usage usage = usage_;
    usage_ = {};
    return usage;
  }

 private:
  std::mutex mutex_;
  Usage usage_;
};

struct ChatResult {
  std::string content;
  std::string reasoning;  // retained only in debug mode
  std::vector<ToolCall> tool_calls;
  json annotations = json::array();
  json usage;  // null unless the server streamed one
  int64_t http_status = 0;
  double first_event_ms = -1;  // first content/reasoning/tool delta
  double duration_ms = -1;     // complete streamed request
  std::string finish_reason;
  std::string error;  // non-empty on failure
  bool interrupted = false;
  bool suppressed =
      false;  // content looked like a text-protocol call; not printed
};

// text-protocol delimiters (shared with agent.h's parser)
inline constexpr const char* kTtOpen = "[uagent_tool_call]";
inline constexpr const char* kTtClose = "[/uagent_tool_call]";

// Incremental SSE parser; prints content (and dim reasoning) as it streams.
struct StreamCtx {
  CURL* handle = nullptr;
  ChatResult* res = nullptr;
  std::string buf;         // partial SSE line
  std::string error_body;  // body when HTTP status >= 400
  int64_t status = 0;
  bool in_reasoning = false;
  std::map<int, ToolCall> calls;  // keyed by stream index
  TerminalSpinner* spinner = nullptr;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point last_byte;
  int64_t first_event_timeout_s = 120;
  int64_t idle_timeout_s = 90;
  size_t response_cap = 32 * 1024 * 1024;
  size_t received = 0;
  std::string timeout_reason;
  MdStream md;  // renders streamed content as ANSI-styled markdown (TTY only)

  // Hold content back while it could still be a text-protocol tool call, so
  // raw [uagent_tool_call] blocks never flash on screen. UNDECIDED until the
  // first non-whitespace bytes either match TT_OPEN (SUPPRESS) or don't
  // (PRINT).
  enum class Show { kUndecided, kPrint, kSuppress } show = Show::kUndecided;

  void MarkEvent() {
    if (res->first_event_ms < 0) res->first_event_ms = ElapsedMs(started);
  }

  void AddAnnotations(const json& annotations) {
    if (!annotations.is_array()) return;
    for (const json& annotation : annotations) {
      if (annotation.is_object()) res->annotations.push_back(annotation);
    }
  }

  void BeginOutput() {  // stop the spinner before any visible bytes
    if (spinner) spinner->Stop();
  }

  void OutputText(const std::string& c) {
    BeginOutput();
    if (in_reasoning) {
      printf("%s\n", RST());
      in_reasoning = false;
    }
    md.Feed(TerminalSafe(c));
  }

  void OutputReasoning(const std::string& r) {
    BeginOutput();
    fputs(DIM(), stdout);
    in_reasoning = true;
    std::string safe = TerminalSafe(r);
    fputs(safe.c_str(), stdout);
    fflush(stdout);
  }

  void EmitContent(const std::string& c) {
    res->content += c;
    if (show == Show::kPrint) {
      OutputText(c);
      return;
    }
    if (show == Show::kSuppress) return;
    const std::string& full = res->content;
    size_t start = full.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return;  // only whitespace so far
    std::string vis = full.substr(start);
    size_t tl = strlen(kTtOpen);
    if (vis.size() >= tl) {
      show = vis.compare(0, tl, kTtOpen) == 0 ? Show::kSuppress : Show::kPrint;
      if (show == Show::kPrint) {
        OutputText(full);
      } else {
        res->suppressed = true;
      }
    } else if (std::string(kTtOpen).compare(0, vis.size(), vis) != 0) {
      show = Show::kPrint;
      OutputText(full);
    }  // else: still a prefix of the tag — keep holding
  }

  // This runs inside a libcurl callback, so malformed server JSON is validated
  // explicitly and never crosses the C boundary.
  void HandleLine(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.starts_with("data:")) return;  // ignore comments/keep-alives
    std::string payload = Trim(line.substr(5));
    if (payload.empty() || payload == "[DONE]") return;
    json j = json::parse(payload, nullptr, false);
    if (j.is_discarded()) return;
    if (j.contains("usage") && !j["usage"].is_null()) res->usage = j["usage"];
    if (!j.contains("choices") || !j["choices"].is_array() ||
        j["choices"].empty()) {
      return;
    }
    const json& ch = j["choices"][0];
    if (!ch.is_object()) return;
    if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) {
      res->finish_reason = ch["finish_reason"].get<std::string>();
    }
    if (ch.contains("annotations")) AddAnnotations(ch["annotations"]);
    if (ch.contains("message") && ch["message"].is_object() &&
        ch["message"].contains("annotations")) {
      AddAnnotations(ch["message"]["annotations"]);
    }
    if (!ch.contains("delta") || !ch["delta"].is_object()) return;
    const json& d = ch["delta"];
    if (d.contains("annotations")) AddAnnotations(d["annotations"]);
    // dim "thinking" text from reasoning models (MiniMax, DeepSeek-R1, ...)
    if (d.contains("reasoning_content") && d["reasoning_content"].is_string()) {
      std::string r = d["reasoning_content"].get<std::string>();
      if (!r.empty()) {
        MarkEvent();
        if (g_debug.Enabled()) res->reasoning += r;
        OutputReasoning(r);
      }
    }
    if (d.contains("content") && d["content"].is_string()) {
      std::string c = d["content"].get<std::string>();
      if (!c.empty()) {
        MarkEvent();
        EmitContent(c);
      }
    }
    if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
      if (!d["tool_calls"].empty()) MarkEvent();
      for (const json& tc : d["tool_calls"]) {
        if (!tc.is_object()) continue;
        ToolCall& slot = calls[JsonInt(tc, "index")];
        if (tc.contains("id") && tc["id"].is_string()) {
          slot.id += tc["id"].get<std::string>();
        }
        if (tc.contains("function") && tc["function"].is_object()) {
          const json& fn = tc["function"];
          if (fn.contains("name") && fn["name"].is_string()) {
            slot.name += fn["name"].get<std::string>();
          }
          if (fn.contains("arguments") && fn["arguments"].is_string()) {
            slot.args += fn["arguments"].get<std::string>();
          }
        }
      }
    }
  }

  size_t Feed(const char* data, size_t len) {
    last_byte = std::chrono::steady_clock::now();
    received += len;
    if (response_cap > 0 && received > response_cap) {
      res->error =
          "response exceeded " + std::to_string(response_cap) + " bytes";
      return 0;
    }
    if (status == 0) curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 400) {
      error_body.append(data, len);
      return len;
    }
    buf.append(data, len);
    size_t start = 0, pos;
    while ((pos = buf.find('\n', start)) != std::string::npos) {
      HandleLine(buf.substr(start, pos - start));
      start = pos + 1;
    }
    buf.erase(0, start);
    return len;
  }

  void Finish() {  // flush a final line that arrived without a trailing newline
    if (!buf.empty()) {
      HandleLine(buf);
      buf.clear();
    }
  }
};

class Api {
 public:
  std::string base_url, api_key, model, reasoning_effort;
  int64_t ctx_window = 0;    // model context size in tokens (0 = unknown)
  bool native_tools = true;  // dropped after a 400 that rejects `tools`
  bool include_usage =
      true;  // dropped after a 400 that rejects `stream_options`
  bool parallel_tools =
      true;  // omit after a 400 that rejects `parallel_tool_calls`
  bool openrouter_web_search =
      true;  // dropped after a 400 that rejects the beta server tool

  explicit Api(RuntimeConfig config = RuntimeConfig::FromEnvironment())
      : config(std::move(config)), handle_(curl_easy_init()) {}
  ~Api() {
    if (handle_) curl_easy_cleanup(handle_);
  }
  Api(const Api&) = delete;
  Api& operator=(const Api&) = delete;
  RuntimeConfig config;

  json BuildChatBody(const json& messages, const json& tool_schemas,
                     const std::string& session_id = "",
                     bool* web_available = nullptr) const {
    if (web_available) *web_available = false;
    json body = {{"model", model}, {"messages", messages}, {"stream", true}};
    if (native_tools && !tool_schemas.empty()) {
      json request_tools = json::array();
      bool server_search = OpenrouterCompatibleUrl(base_url) &&
                           config.web_search_server && openrouter_web_search;
      bool search_offered = false;
      for (const json& tool : tool_schemas) {
        bool legacy_search =
            tool.is_object() && tool.contains("function") &&
            tool["function"].is_object() &&
            JsonString(tool["function"], "name") == "web_search";
        search_offered = search_offered || legacy_search;
        if (legacy_search &&
            (server_search || !OpenrouterCompatibleUrl(base_url))) {
          continue;
        }
        request_tools.push_back(tool);
      }
      if (server_search && search_offered) {
        if (web_available) *web_available = true;
        json parameters = {
            {"engine", config.web_search_engine},
            {"max_results", config.web_search_max_results},
            {"max_uses", config.web_search_max_uses},
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
    if (include_usage) body["stream_options"] = {{"include_usage", true}};
    int64_t max_tokens = MaxOutputTokens();
    if (max_tokens > 0) {
      body[OpenaiUrl(base_url) ? "max_completion_tokens" : "max_tokens"] =
          max_tokens;
    }
    if (!reasoning_effort.empty()) {
      if (OpenrouterUrl(base_url)) {
        body["reasoning"] = {{"effort", reasoning_effort}};
      } else {
        body["reasoning_effort"] = reasoning_effort;
      }
    }
    if (OpenrouterUrl(base_url)) {
      if (!session_id.empty()) body["session_id"] = session_id;
      if (!config.openrouter_provider.empty()) {
        body["provider"] = {
            {"order", json::array({config.openrouter_provider})},
            {"allow_fallbacks", config.openrouter_fallbacks}};
      }
    }
    return body;
  }

  ChatResult Chat(const json& messages, const json& tool_schemas,
                  int64_t timeout_s = 0, const std::string& session_id = "") {
    ChatResult res;
    size_t estimated =
        JsonEstimatedBytes(messages) + JsonEstimatedBytes(tool_schemas);
    if (config.request_bytes > 0 &&
        estimated > static_cast<size_t>(config.request_bytes)) {
      res.error =
          "request exceeds " + std::to_string(config.request_bytes) + " bytes";
      return res;
    }
    bool web_available = false;
    json body =
        BuildChatBody(messages, tool_schemas, session_id, &web_available);
    std::string payload = JsonDump(body);
    if (config.request_bytes > 0 &&
        payload.size() > static_cast<size_t>(config.request_bytes)) {
      res.error = "serialized request exceeds " +
                  std::to_string(config.request_bytes) + " bytes";
      return res;
    }

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
    int64_t response_cap = config.response_bytes;
    ctx.response_cap = response_cap > 0 ? static_cast<size_t>(response_cap) : 0;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs =
        curl_slist_append(hdrs, ("Authorization: Bearer " + api_key).c_str());
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    if (OpenrouterUrl(base_url) && !session_id.empty()) {
      hdrs = curl_slist_append(hdrs, ("X-Session-Id: " + session_id).c_str());
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(
        h, CURLOPT_WRITEFUNCTION,
        +[](char* d, size_t s, size_t n, void* u) -> size_t {
          return static_cast<StreamCtx*>(u)->Feed(d, s * n);
        });
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    SetAbortable(h, &ctx);
    int64_t request_timeout =
        timeout_s > 0 ? timeout_s : config.request_timeout_s;
    if (request_timeout > 0) {
      curl_easy_setopt(h, CURLOPT_TIMEOUT, request_timeout);
    }

    SteeringGuard steering;
    std::string activity =
        web_available ? "working · web available" : "working";
    TerminalSpinner spinner(true, SpinnerLabel(std::move(activity)));
    ctx.spinner = &spinner;

    CURLcode rc = CURLE_OK;
    bool cancelled = RunCancellable([&] { rc = curl_easy_perform(h); });
    steering.Stop();
    res.duration_ms = ElapsedMs(ctx.started);
    ctx.BeginOutput();  // stops the spinner if nothing was ever printed
    ctx.Finish();
    if (ctx.show == StreamCtx::Show::kUndecided && !res.content.empty()) {
      ctx.OutputText(res.content);  // held fragment that never resolved
    }
    ctx.md
        .Flush();  // render anything still held (open styles, a trailing table)
    if (ctx.in_reasoning) printf("%s\n", RST());
    curl_slist_free_all(hdrs);

    if (!ctx.timeout_reason.empty()) {
      res.error = ctx.timeout_reason;
      return res;
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK || cancelled) {
      res.interrupted = true;
      return res;
    }
    if (!res.error.empty()) return res;
    if (rc != CURLE_OK) {
      res.error = std::string("connection error: ") + curl_easy_strerror(rc);
      return res;
    }
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE,
                      &res.http_status);  // even bodiless replies
    if (res.http_status >= 400) {
      std::string msg = ctx.error_body;
      json error_response = json::parse(ctx.error_body, nullptr, false);
      if (error_response.is_object() && error_response.contains("error")) {
        const json& error = error_response["error"];
        if (error.is_string()) {
          msg = error.get<std::string>();
        } else if (error.is_object()) {
          msg = JsonString(error, "message", msg);
        }
      }
      res.error = "HTTP " + std::to_string(res.http_status) + ": " + msg;
      return res;
    }
    for (auto& [idx, tc] : ctx.calls) {
      if (!tc.name.empty()) res.tool_calls.push_back(tc);
    }
    return res;
  }

  // quiet JSON POST — no streaming, no printing; Ctrl+C cancels.
  // Used by the web_search tool's side-request.
  json Post(const std::string& path, const json& body, int64_t timeout_s = 120,
            const std::atomic<bool>* cancel = nullptr) {
    std::string payload = JsonDump(body);
    return Fetch(path, &payload, timeout_s, /*abortable=*/true, cancel);
  }

  // GET base_url+path, parsed JSON (discarded value on failure)
  json Get(const std::string& path) { return Fetch(path, nullptr, 15, false); }

 private:
  // shared non-streaming request: POSTs payload when given, else GETs;
  // returns parsed JSON (discarded value on failure)
  json Fetch(const std::string& path, const std::string* payload,
             int64_t timeout_s, bool abortable,
             const std::atomic<bool>* cancel = nullptr) {
    CURL* h = Prepare(base_url + path);
    if (!h) return json(json::value_t::discarded);
    struct FetchBuffer {
      std::string data;
      size_t cap = 0;
      bool exceeded = false;
    } out;
    int64_t configured_cap = config.response_bytes;
    out.cap = configured_cap > 0 ? static_cast<size_t>(configured_cap) : 0;
    struct curl_slist* hdrs = nullptr;
    hdrs =
        curl_slist_append(hdrs, ("Authorization: Bearer " + api_key).c_str());
    if (payload) {
      hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload->c_str());
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(payload->size()));
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(
        h, CURLOPT_WRITEFUNCTION,
        +[](char* d, size_t s, size_t n, void* u) -> size_t {
          auto* out = static_cast<FetchBuffer*>(u);
          size_t bytes = s * n;
          if (out->cap > 0 && out->data.size() + bytes > out->cap) {
            out->exceeded = true;
            return 0;
          }
          out->data.append(d, bytes);
          return bytes;
        });
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    if (abortable) {
      if (cancel) {
        SetCancelable(h, cancel);
      } else {
        SetAbortable(h);
      }
    }
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING,
                     "");  // 531 KB -> 63 KB on /models
    CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(hdrs);
    if (rc != CURLE_OK || out.exceeded) return json(json::value_t::discarded);
    return json::parse(out.data, nullptr, false);
  }

  // Ctrl+C aborts the transfer via a nonzero progress callback.
  static void SetAbortable(CURL* h, StreamCtx* ctx = nullptr) {
    curl_easy_setopt(
        h, CURLOPT_XFERINFOFUNCTION,
        +[](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
          if (AbortRequested()) return 1;
          auto* ctx = static_cast<StreamCtx*>(user);
          if (!ctx) return 0;
          auto now = std::chrono::steady_clock::now();
          if (ctx->res->first_event_ms < 0 && ctx->first_event_timeout_s > 0 &&
              now - ctx->started >=
                  std::chrono::seconds(ctx->first_event_timeout_s)) {
            ctx->timeout_reason = "model produced no event within " +
                                  std::to_string(ctx->first_event_timeout_s) +
                                  "s";
            return 1;
          }
          if (ctx->idle_timeout_s > 0 &&
              now - ctx->last_byte >=
                  std::chrono::seconds(ctx->idle_timeout_s)) {
            ctx->timeout_reason = "model stream was idle for " +
                                  std::to_string(ctx->idle_timeout_s) + "s";
            return 1;
          }
          return 0;
        });
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  }

  static void SetCancelable(CURL* h, const std::atomic<bool>* cancel) {
    curl_easy_setopt(
        h, CURLOPT_XFERINFOFUNCTION,
        +[](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
          const auto* cancel = static_cast<const std::atomic<bool>*>(user);
          return AbortRequested() || cancel->load();
        });
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  }

  // One easy handle for the Api's lifetime: libcurl keeps the TCP/TLS
  // connection alive between requests, which matters on slow hardware.
  CURL* Prepare(const std::string& url) {
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

  CURL* handle_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_H_
