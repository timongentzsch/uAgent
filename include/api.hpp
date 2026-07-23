#pragma once
// OpenAI-compatible streaming client: libcurl + SSE. Speaks to any endpoint
// (llama.cpp, vLLM, SGLang, OpenAI, Z.ai, ...) and degrades gracefully when
// a server rejects `tools` or `stream_options`.

#include <curl/curl.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "md.hpp"
#include "tools.hpp"
#include "util.hpp"

using nlohmann::json;

struct ToolCall {
    std::string id, name, args;  // args: raw JSON string
};

struct Usage {
    long input = 0, output = 0, cache_read = 0, reasoning = 0;
    double cost = 0;  // credits (~USD), e.g. OpenRouter's `usage.cost`; 0 = not reported
    long cache_write = 0;
    void merge(const Usage& o) {
        input += o.input; output += o.output; cache_read += o.cache_read;
        reasoning += o.reasoning; cost += o.cost; cache_write += o.cache_write;
    }
    // OpenAI convention: input excludes cached tokens, output excludes reasoning.
    // Guarded: a server reporting a token count as a string must not abort the turn.
    void add(const json& u) try {
        if (!u.is_object()) return;
        auto detail = [&](const char* k, const char* f) {
            return u.contains(k) && u[k].is_object() ? u[k].value(f, 0L) : 0L;
        };
        long cache = detail("prompt_tokens_details", "cached_tokens");
        long cache_write_tokens =
            detail("prompt_tokens_details", "cache_write_tokens");
        if (!cache_write_tokens)
            cache_write_tokens = detail("cache_details", "cache_write_tokens");
        if (!cache_write_tokens)
            cache_write_tokens = u.value("cache_write_tokens", 0L);
        long reason = detail("completion_tokens_details", "reasoning_tokens");
        input += u.value("prompt_tokens", 0L) - cache;
        output += u.value("completion_tokens", 0L) - reason;
        cache_read += cache;
        cache_write += cache_write_tokens;
        reasoning += reason;
        cost += u.value("cost", 0.0);
    } catch (const json::exception&) {}
};

inline json usage_json(const Usage& usage) {
    return {{"input", usage.input},
            {"output", usage.output},
            {"cache_read", usage.cache_read},
            {"cache_write", usage.cache_write},
            {"reasoning", usage.reasoning},
            {"cost", usage.cost}};
}

// inverse of usage_json — reads back a total this or another process wrote
inline Usage usage_from_json(const json& j) {
    Usage u;
    if (!j.is_object()) return u;
    u.input = j.value("input", 0L);
    u.output = j.value("output", 0L);
    u.cache_read = j.value("cache_read", 0L);
    u.cache_write = j.value("cache_write", 0L);
    u.reasoning = j.value("reasoning", 0L);
    u.cost = j.value("cost", 0.0);
    return u;
}

// Usage from concurrent side-requests (web_search) and subagent processes.
// This is session-owned rather than process-global so independent Agent
// instances cannot accidentally bill one another.
class UsageAccumulator {
public:
    void add(const json& usage) {
        std::lock_guard<std::mutex> lock(mutex_);
        usage_.add(usage);
    }
    void add(const Usage& usage) {
        std::lock_guard<std::mutex> lock(mutex_);
        usage_.merge(usage);
    }
    Usage take() {
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
    std::string reasoning;       // retained only in debug mode
    std::vector<ToolCall> tool_calls;
    json usage;             // null unless the server streamed one
    long http_status = 0;
    double first_event_ms = -1;  // first content/reasoning/tool delta
    double duration_ms = -1;     // complete streamed request
    std::string finish_reason;
    std::string error;      // non-empty on failure
    bool interrupted = false;
    bool suppressed = false;  // content looked like a text-protocol call; not printed
};

// text-protocol delimiters (shared with agent.hpp's parser)
inline constexpr const char* TT_OPEN = "[uagent_tool_call]";
inline constexpr const char* TT_CLOSE = "[/uagent_tool_call]";

// Incremental SSE parser; prints content (and dim reasoning) as it streams.
// A spinner thread animates until the first visible byte; `spin_mtx` guards
// only that one-time handoff.
struct StreamCtx {
    CURL* handle = nullptr;
    ChatResult* res = nullptr;
    std::string buf;         // partial SSE line
    std::string error_body;  // body when HTTP status >= 400
    long status = 0;
    bool in_reasoning = false;
    std::map<int, ToolCall> calls;  // keyed by stream index
    std::atomic<bool> printed{false};
    std::mutex spin_mtx;
    bool spinner_drawn = false;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point last_byte;
    long first_event_timeout_s = 120;
    long idle_timeout_s = 90;
    size_t response_cap = 32 * 1024 * 1024;
    size_t received = 0;
    std::string timeout_reason;
    MdStream md;  // renders streamed content as ANSI-styled markdown (TTY only)

    // Hold content back while it could still be a text-protocol tool call, so
    // raw [uagent_tool_call] blocks never flash on screen. UNDECIDED until the
    // first non-whitespace bytes either match TT_OPEN (SUPPRESS) or don't (PRINT).
    enum class Show { UNDECIDED, PRINT, SUPPRESS } show = Show::UNDECIDED;

    void mark_event() {
        if (res->first_event_ms < 0)
            res->first_event_ms = elapsed_ms(started);
    }

    void begin_output() {  // stop the spinner, exactly once, before visible bytes
        if (printed) return;
        std::lock_guard<std::mutex> lk(spin_mtx);
        printed = true;
        if (spinner_drawn) fputs("\r\033[K", stdout);
    }

    void output_text(const std::string& c) {
        begin_output();
        if (in_reasoning) { printf("%s\n", RST()); in_reasoning = false; }
        md.feed(terminal_safe(c));
    }

    void output_reasoning(const std::string& r) {
        begin_output();
        fputs(DIM(), stdout);
        in_reasoning = true;
        std::string safe = terminal_safe(r);
        fputs(safe.c_str(), stdout);
        fflush(stdout);
    }

    void emit_content(const std::string& c) {
        res->content += c;
        if (show == Show::PRINT) { output_text(c); return; }
        if (show == Show::SUPPRESS) return;
        const std::string& full = res->content;
        size_t start = full.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return;  // only whitespace so far
        std::string vis = full.substr(start);
        size_t tl = strlen(TT_OPEN);
        if (vis.size() >= tl) {
            show = vis.compare(0, tl, TT_OPEN) == 0 ? Show::SUPPRESS : Show::PRINT;
            if (show == Show::PRINT) output_text(full);
            else res->suppressed = true;
        } else if (std::string(TT_OPEN).compare(0, vis.size(), vis) != 0) {
            show = Show::PRINT;
            output_text(full);
        }  // else: still a prefix of the tag — keep holding
    }

    // never throws — this runs inside a libcurl callback, and malformed server
    // JSON (e.g. a non-integer `index`) must not unwind through C code
    void handle_line(std::string line) try {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) != 0) return;  // ignore comments/keep-alives
        std::string payload = trim(line.substr(5));
        if (payload.empty() || payload == "[DONE]") return;
        json j = json::parse(payload, nullptr, false);
        if (j.is_discarded()) return;
        if (j.contains("usage") && !j["usage"].is_null()) res->usage = j["usage"];
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
        const json& ch = j["choices"][0];
        if (ch.contains("finish_reason") && ch["finish_reason"].is_string())
            res->finish_reason = ch["finish_reason"].get<std::string>();
        if (!ch.contains("delta")) return;
        const json& d = ch["delta"];
        // dim "thinking" text from reasoning models (MiniMax, DeepSeek-R1, ...)
        if (d.contains("reasoning_content") && d["reasoning_content"].is_string()) {
            std::string r = d["reasoning_content"].get<std::string>();
            if (!r.empty()) {
                mark_event();
                if (g_debug.enabled()) res->reasoning += r;
                output_reasoning(r);
            }
        }
        if (d.contains("content") && d["content"].is_string()) {
            std::string c = d["content"].get<std::string>();
            if (!c.empty()) { mark_event(); emit_content(c); }
        }
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
            if (!d["tool_calls"].empty()) mark_event();
            for (const json& tc : d["tool_calls"]) {
                ToolCall& slot = calls[tc.value("index", 0)];
                if (tc.contains("id") && tc["id"].is_string())
                    slot.id += tc["id"].get<std::string>();
                if (tc.contains("function") && tc["function"].is_object()) {
                    const json& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string())
                        slot.name += fn["name"].get<std::string>();
                    if (fn.contains("arguments") && fn["arguments"].is_string())
                        slot.args += fn["arguments"].get<std::string>();
                }
            }
        }
    } catch (const json::exception&) { /* drop the malformed event */ }

    size_t feed(const char* data, size_t len) {
        last_byte = std::chrono::steady_clock::now();
        received += len;
        if (response_cap > 0 && received > response_cap) {
            res->error = "response exceeded " + std::to_string(response_cap) + " bytes";
            return 0;
        }
        if (status == 0) curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 400) { error_body.append(data, len); return len; }
        buf.append(data, len);
        size_t start = 0, pos;
        while ((pos = buf.find('\n', start)) != std::string::npos) {
            handle_line(buf.substr(start, pos - start));
            start = pos + 1;
        }
        buf.erase(0, start);
        return len;
    }

    void finish() {  // flush a final line that arrived without a trailing newline
        if (!buf.empty()) { handle_line(buf); buf.clear(); }
    }
};

class Api {
public:
    std::string base_url, api_key, model, reasoning_effort;
    long ctx_window = 0;        // model context size in tokens (0 = unknown)
    bool native_tools = true;   // dropped after a 400 that rejects `tools`
    bool include_usage = true;  // dropped after a 400 that rejects `stream_options`
    bool parallel_tools = true; // omit after a 400 that rejects `parallel_tool_calls`

    explicit Api(RuntimeConfig config = RuntimeConfig::from_environment())
        : config(std::move(config)), handle_(curl_easy_init()) {}
    ~Api() { if (handle_) curl_easy_cleanup(handle_); }
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;
    RuntimeConfig config;

    json build_chat_body(const json& messages, const json& tool_schemas,
                         const std::string& session_id = "") const {
        json body = {{"model", model}, {"messages", messages}, {"stream", true}};
        if (native_tools && !tool_schemas.empty()) {
            body["tools"] = tool_schemas;
            if (parallel_tools) body["parallel_tool_calls"] = true;
        }
        if (include_usage) body["stream_options"] = {{"include_usage", true}};
        long max_tokens = max_output_tokens();
        if (max_tokens > 0)
            body[openai_url(base_url) ? "max_completion_tokens" : "max_tokens"] = max_tokens;
        if (!reasoning_effort.empty()) {
            if (openrouter_url(base_url)) {
                body["reasoning"] = {{"effort", reasoning_effort}};
            } else {
                body["reasoning_effort"] = reasoning_effort;
            }
        }
        if (openrouter_url(base_url)) {
            if (!session_id.empty()) body["session_id"] = session_id;
            if (!config.openrouter_provider.empty())
                body["provider"] = {
                    {"order", json::array({config.openrouter_provider})},
                    {"allow_fallbacks", config.openrouter_fallbacks}};
        }
        return body;
    }

    ChatResult chat(const json& messages, const json& tool_schemas,
                    long timeout_s = 0, const std::string& session_id = "") {
        ChatResult res;
        size_t estimated = json_estimated_bytes(messages) +
                           json_estimated_bytes(tool_schemas);
        if (config.request_bytes > 0 &&
            estimated > static_cast<size_t>(config.request_bytes)) {
            res.error = "request exceeds " +
                        std::to_string(config.request_bytes) + " bytes";
            return res;
        }
        json body = build_chat_body(messages, tool_schemas, session_id);
        std::string payload = body.dump();
        if (config.request_bytes > 0 &&
            payload.size() > static_cast<size_t>(config.request_bytes)) {
            res.error = "serialized request exceeds " +
                        std::to_string(config.request_bytes) + " bytes";
            return res;
        }

        CURL* h = prepare(base_url + "/chat/completions");
        if (!h) { res.error = "curl init failed"; return res; }
        StreamCtx ctx;
        ctx.handle = h;
        ctx.res = &res;
        ctx.started = std::chrono::steady_clock::now();
        ctx.last_byte = ctx.started;
        ctx.first_event_timeout_s = config.first_event_timeout_s;
        ctx.idle_timeout_s = config.stream_idle_timeout_s;
        long response_cap = config.response_bytes;
        ctx.response_cap = response_cap > 0 ? static_cast<size_t>(response_cap) : 0;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + api_key).c_str());
        hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
        if (openrouter_url(base_url) && !session_id.empty())
            hdrs = curl_slist_append(
                hdrs, ("X-Session-Id: " + session_id).c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)payload.size());
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,
                         +[](char* d, size_t s, size_t n, void* u) -> size_t {
                             return static_cast<StreamCtx*>(u)->feed(d, s * n);
                         });
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
        set_abortable(h, &ctx);
        long request_timeout = timeout_s > 0 ? timeout_s : config.request_timeout_s;
        if (request_timeout > 0) curl_easy_setopt(h, CURLOPT_TIMEOUT, request_timeout);

        SteeringGuard steering;
        // ascii spinner until the first visible byte (TTY only)
        std::thread spinner;
        if (g_tty)
            spinner = std::thread([&ctx] {
                for (int i = 0; !ctx.printed; i = (i + 1) & 3) {
                    {
                        std::lock_guard<std::mutex> lk(ctx.spin_mtx);
                        if (ctx.printed) break;
                        printf("\r%s%c%s", DIM(), "|/-\\"[i], RST());
                        fflush(stdout);
                        ctx.spinner_drawn = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });

        CURLcode rc = CURLE_OK;
        bool cancelled = run_cancellable([&] { rc = curl_easy_perform(h); });
        steering.stop();
        res.duration_ms = elapsed_ms(ctx.started);
        ctx.begin_output();  // stops the spinner if nothing was ever printed
        if (spinner.joinable()) spinner.join();
        ctx.finish();
        if (ctx.show == StreamCtx::Show::UNDECIDED && !res.content.empty())
            ctx.output_text(res.content);  // held fragment that never resolved
        ctx.md.flush();  // render anything still held (open styles, a trailing table)
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
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.http_status);  // even bodiless replies
        if (res.http_status >= 400) {
            std::string msg = ctx.error_body;
            try {
                json j = json::parse(ctx.error_body);
                const json& e = j.at("error");
                msg = e.is_string() ? e.get<std::string>() : e.at("message").get<std::string>();
            } catch (...) {}
            res.error = "HTTP " + std::to_string(res.http_status) + ": " + msg;
            return res;
        }
        for (auto& [idx, tc] : ctx.calls)
            if (!tc.name.empty()) res.tool_calls.push_back(tc);
        return res;
    }

    // quiet JSON POST — no streaming, no printing; Ctrl+C cancels.
    // Used by the web_search tool's side-request.
    json post(const std::string& path, const json& body, long timeout_s = 120,
              const std::atomic<bool>* cancel = nullptr) {
        std::string payload = body.dump();
        return fetch(path, &payload, timeout_s, /*abortable=*/true, cancel);
    }

    // GET base_url+path, parsed JSON (discarded value on failure)
    json get(const std::string& path) { return fetch(path, nullptr, 15, false); }

private:
    // shared non-streaming request: POSTs payload when given, else GETs;
    // returns parsed JSON (discarded value on failure)
    json fetch(const std::string& path, const std::string* payload, long timeout_s,
               bool abortable, const std::atomic<bool>* cancel = nullptr) {
        CURL* h = prepare(base_url + path);
        if (!h) return json(json::value_t::discarded);
        struct FetchBuffer {
            std::string data;
            size_t cap = 0;
            bool exceeded = false;
        } out;
        long configured_cap = config.response_bytes;
        out.cap = configured_cap > 0 ? static_cast<size_t>(configured_cap) : 0;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + api_key).c_str());
        if (payload) {
            hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload->c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)payload->size());
        }
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,
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
            if (cancel) set_cancelable(h, cancel);
            else set_abortable(h);
        }
        curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // 531 KB -> 63 KB on /models
        CURLcode rc = curl_easy_perform(h);
        curl_slist_free_all(hdrs);
        if (rc != CURLE_OK || out.exceeded) return json(json::value_t::discarded);
        return json::parse(out.data, nullptr, false);
    }

    // Ctrl+C aborts the transfer via a nonzero progress callback.
    static void set_abortable(CURL* h, StreamCtx* ctx = nullptr) {
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION,
                         +[](void* user, curl_off_t, curl_off_t, curl_off_t,
                             curl_off_t) -> int {
                             if (abort_requested()) return 1;
                             auto* ctx = static_cast<StreamCtx*>(user);
                             if (!ctx) return 0;
                             auto now = std::chrono::steady_clock::now();
                             if (ctx->res->first_event_ms < 0 &&
                                 ctx->first_event_timeout_s > 0 &&
                                 now - ctx->started >=
                                     std::chrono::seconds(ctx->first_event_timeout_s)) {
                                 ctx->timeout_reason =
                                     "model produced no event within " +
                                     std::to_string(ctx->first_event_timeout_s) + "s";
                                 return 1;
                             }
                             if (ctx->idle_timeout_s > 0 &&
                                 now - ctx->last_byte >=
                                     std::chrono::seconds(ctx->idle_timeout_s)) {
                                 ctx->timeout_reason =
                                     "model stream was idle for " +
                                     std::to_string(ctx->idle_timeout_s) + "s";
                                 return 1;
                             }
                             return 0;
                         });
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, ctx);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    }

    static void set_cancelable(CURL* h, const std::atomic<bool>* cancel) {
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION,
                         +[](void* user, curl_off_t, curl_off_t, curl_off_t,
                             curl_off_t) -> int {
                             const auto* cancel =
                                 static_cast<const std::atomic<bool>*>(user);
                             return abort_requested() || cancel->load();
                         });
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, cancel);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    }

    // One easy handle for the Api's lifetime: libcurl keeps the TCP/TLS
    // connection alive between requests, which matters on slow hardware.
    CURL* prepare(const std::string& url) {
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
        curl_easy_setopt(handle_, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        return handle_;
    }

    CURL* handle_;
};
