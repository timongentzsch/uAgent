#pragma once
// Minimal MCP (Model Context Protocol) client — stdio transport only.
// Speaks just the JSON-RPC subset a coding agent needs: initialize,
// tools/list, tools/call. Each configured server becomes ordinary registry
// Tools whose run() forwards to tools/call — nothing in the agent loop
// changes. Resources, prompts, sampling and HTTP transports are deliberately
// not implemented.
//
// Config: `~/.mcp.json`, plus an explicitly trusted project `.mcp.json` — the standard
// format shared with Claude Code / Cursor / VS Code:
//   {"mcpServers": {"github": {"command": "npx", "args": ["-y", "..."],
//                              "env": {"TOKEN": "$github_pat"}}}}
// µAgent extensions per server (both optional):
//   "tools": ["a", "b"]  register only these tools (big servers ship dozens
//                        of tools; unused schemas are pure input-token cost)
//   "trust": true        honor the server's readOnlyHint annotation — such
//                        tools skip the approval prompt
//
// Token hygiene: descriptions are capped (UAGENT_MCP_DESC_CHARS), registry
// size is bounded, and the per-server schema weight is printed at startup.
// Input schemas themselves remain semantically unchanged from the server's
// declaration so validation constraints are never weakened.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "json.hpp"
#include "tools.hpp"
#include "util.hpp"

using nlohmann::json;

// --- server process ----------------------------------------------------------

struct McpServer {
    std::string name;
    pid_t pid = -1;
    int in = -1, out = -1;  // in: we write (server's stdin) · out: we read (its stdout)
    bool alive = false;
    std::string rbuf;       // partial line from the server
    long next_id = 1;
    size_t response_cap = 16 * 1024 * 1024;
    json config;
    bool tools_changed = false;

    ~McpServer() { shutdown(); }
    // stdin EOF is the polite stop signal for stdio servers; escalate to
    // SIGTERM, then SIGKILL for the ones that don't watch their stdin.
    // Signals target the whole group (-pid): servers spawn their own workers.
    // Also called the moment a server is detected dead/wedged, so fds close
    // and the child is reaped immediately, not at program exit.
    void shutdown() {
        alive = false;
        if (in >= 0) { close(in); in = -1; }
        if (out >= 0) { close(out); out = -1; }
        if (pid > 0) {
            int st;
            bool gone = false;
            // Give stdio EOF a brief chance to perform the protocol's polite
            // shutdown before escalating to signals.
            for (int i = 0; i < 4 && !gone; i++) {
                if (waitpid(pid, &st, WNOHANG) != 0) gone = true;
                else usleep(50 * 1000);
            }
            if (!gone && kill(-pid, SIGTERM) != 0) kill(pid, SIGTERM);
            for (int i = 0; i < 6 && !gone; i++) {
                if (waitpid(pid, &st, WNOHANG) != 0) gone = true;
                else usleep(50 * 1000);
            }
            if (!gone) { kill(-pid, SIGKILL); kill(pid, SIGKILL); waitpid(pid, &st, 0); }
            for (int i = 0; i < MCP_MAX; i++)  // drop from the SIGINT kill list
                if (g_mcp_pids[i] == pid) g_mcp_pids[i] = 0;
            pid = -1;
        }
    }
};

inline constexpr const char* CHROME_MCP_NAME = "chrome-devtools";
inline constexpr const char* CHROME_MCP_PACKAGE = "chrome-devtools-mcp@latest";

inline json chrome_mcp_config(const std::string& mode = "isolated") {
    json args = json::array(
        {"-y", CHROME_MCP_PACKAGE, "--no-usage-statistics", "--no-performance-crux"});
    args.push_back(mode == "user" ? "--auto-connect" : "--isolated");
    return {{"command", "npx"},
            {"args", std::move(args)},
            {"__uagent_builtin", "chrome-devtools"},
            {"__uagent_mode", mode}};
}

// Explicit session owner. Destruction closes every transport and reaps every
// server before curl and the rest of the process runtime are torn down.
class McpRuntime {
public:
    McpRuntime() = default;
    ~McpRuntime() { shutdown_all(); }
    McpRuntime(const McpRuntime&) = delete;
    McpRuntime& operator=(const McpRuntime&) = delete;

    McpServer* add(std::unique_ptr<McpServer> server) {
        McpServer* raw = server.get();
        servers_.push_back(std::move(server));
        return raw;
    }

    size_t size() const { return servers_.size(); }
    const std::vector<std::unique_ptr<McpServer>>& servers() const {
        return servers_;
    }

private:
    void shutdown_all() {
        auto reaped = [](McpServer& server) {
            for (int i = 0; i < MCP_MAX; ++i)
                if (g_mcp_pids[i] == server.pid) g_mcp_pids[i] = 0;
            server.pid = -1;
        };
        auto reap_ready = [&](McpServer& server) {
            if (server.pid <= 0) return true;
            int status = 0;
            pid_t result = waitpid(server.pid, &status, WNOHANG);
            if (result == server.pid || (result < 0 && errno == ECHILD)) {
                reaped(server);
                return true;
            }
            return false;
        };
        auto wait_all = [&](int attempts) {
            for (int attempt = 0; attempt < attempts; ++attempt) {
                bool pending = false;
                for (auto& server : servers_)
                    pending = !reap_ready(*server) || pending;
                if (!pending) return;
                usleep(50 * 1000);
            }
        };

        // Close every transport first so all servers observe EOF together.
        for (auto& server : servers_) {
            server->alive = false;
            if (server->in >= 0) { close(server->in); server->in = -1; }
            if (server->out >= 0) { close(server->out); server->out = -1; }
        }
        wait_all(4);
        for (auto& server : servers_)
            if (server->pid > 0 && kill(-server->pid, SIGTERM) != 0)
                kill(server->pid, SIGTERM);
        wait_all(6);
        for (auto& server : servers_)
            if (server->pid > 0) {
                kill(-server->pid, SIGKILL);
                kill(server->pid, SIGKILL);
                int status = 0;
                while (waitpid(server->pid, &status, 0) < 0 && errno == EINTR) {}
                reaped(*server);
            }
        servers_.clear();
    }

    std::vector<std::unique_ptr<McpServer>> servers_;
};

// One voice for server status: notes are dim and bulleted, errors are red.
inline void mcp_note(const std::string& name, const std::string& msg) {
    std::string safe_name = terminal_safe(name);
    std::string safe_msg = terminal_safe(msg);
    printf("%s· mcp: %s — %s%s\n", DIM(), safe_name.c_str(), safe_msg.c_str(), RST());
}
inline void mcp_error(const std::string& name, const std::string& msg) {
    std::string safe_name = terminal_safe(name);
    std::string safe_msg = terminal_safe(msg);
    printf("%smcp: %s — %s%s\n", RED(), safe_name.c_str(), safe_msg.c_str(), RST());
}

inline std::string mcp_log_path(const std::string& name) {
    return uagent_dir("mcp") + "/" + safe_file_component(name) + "-" +
           std::to_string(getpid()) + ".log";
}

inline bool mcp_buffer_ok(McpServer& s) {
    if (s.response_cap == 0 || s.rbuf.size() <= s.response_cap) return true;
    s.shutdown();
    return false;
}

inline bool mcp_spawn(McpServer& s, const std::string& cmd,
                      const std::vector<std::string>& args,
                      const std::vector<std::pair<std::string, std::string>>& env,
                      const std::string& cwd, size_t log_bytes) {
    int inp[2], outp[2];  // inp: us -> server stdin, outp: server stdout -> us
    if (pipe(inp) != 0) return false;
    if (pipe(outp) != 0) { close(inp[0]); close(inp[1]); return false; }
    // parent ends must not leak into later-spawned servers (a leaked write end
    // would keep a sibling's stdin open forever, defeating EOF shutdown)
    fcntl(inp[1], F_SETFD, FD_CLOEXEC);
    fcntl(outp[0], F_SETFD, FD_CLOEXEC);
    // nonblocking writes: a blocking write() of a large request would ignore
    // the drain in mcp_write and reintroduce the two-full-pipes deadlock
    fcntl(inp[1], F_SETFL, O_NONBLOCK);
    int errfd = open(mcp_log_path(s.name).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    pid_t pid = fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        if (errfd >= 0) close(errfd);
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);  // own group: terminal Ctrl+C must not kill the server
        dup2(inp[0], 0);
        dup2(outp[1], 1);
        if (errfd >= 0) { dup2(errfd, 2); close(errfd); }
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        struct rlimit file_limit = {
            static_cast<rlim_t>(log_bytes),
            static_cast<rlim_t>(log_bytes)};
        setrlimit(RLIMIT_FSIZE, &file_limit);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            dprintf(STDERR_FILENO, "cannot chdir to %s: %s\n", cwd.c_str(),
                    strerror(errno));
            _exit(126);
        }
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(cmd.c_str()));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        signal(SIGINT, SIG_DFL);
        execvp(cmd.c_str(), argv.data());
        _exit(127);
    }
    close(inp[0]); close(outp[1]);
    if (errfd >= 0) close(errfd);
    s.pid = pid; s.in = inp[1]; s.out = outp[0]; s.alive = true;
    for (int i = 0; i < MCP_MAX; i++)  // SIGINT idle-exit TERMs these (see util.hpp)
        if (g_mcp_pids[i] == 0) { g_mcp_pids[i] = pid; break; }
    return true;
}

// --- newline-delimited JSON-RPC framing --------------------------------------

// write a full message; drains the server's stdout while blocked so a chatty
// server can never deadlock a large write (both pipes full = classic hang)
inline bool mcp_write(McpServer& s, const std::string& data) {
    if (!s.alive) return false;
    size_t off = 0;
    while (off < data.size()) {
        if (abort_requested()) return false;  // user hit Ctrl+C mid-call
        struct pollfd p[2] = {{s.in, POLLOUT, 0}, {s.out, POLLIN, 0}};
        int pr = poll(p, 2, 10000);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) { s.shutdown(); return false; }  // wedged server: kill it now
        if (p[1].revents & POLLIN) {
            char buf[1 << 16];
            ssize_t n = read(s.out, buf, sizeof buf);
            if (n > 0) {
                s.rbuf.append(buf, (size_t)n);
                if (!mcp_buffer_ok(s)) return false;
            }
        }
        if (p[0].revents & (POLLERR | POLLHUP)) { s.shutdown(); return false; }
        if (p[0].revents & POLLOUT) {
            ssize_t n = write(s.in, data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                s.shutdown();
                return false;
            }
            off += (size_t)n;
        }
    }
    return true;
}

// read one newline-terminated message; cancellable waits also stop on Ctrl+C
inline bool mcp_read_line(McpServer& s, std::string& line,
                          std::chrono::steady_clock::time_point deadline, bool cancellable) {
    for (;;) {
        size_t nl = s.rbuf.find('\n');
        if (nl != std::string::npos) {
            line = s.rbuf.substr(0, nl);
            s.rbuf.erase(0, nl + 1);
            return true;
        }
        if (!s.alive) return false;
        if (cancellable && abort_requested()) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        struct pollfd p = {s.out, POLLIN, 0};
        int pr = poll(&p, 1, 200);
        if (pr < 0 && errno != EINTR) { s.shutdown(); return false; }
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
            char buf[1 << 16];
            ssize_t n = read(s.out, buf, sizeof buf);
            if (n <= 0) { s.shutdown(); return false; }  // EOF: server exited — reap it
            s.rbuf.append(buf, (size_t)n);
            if (!mcp_buffer_ok(s)) return false;
        }
    }
}

// id >= 0: request · id < 0: notification
inline bool mcp_send(McpServer& s, long id, const std::string& method, const json& params) {
    json m = {{"jsonrpc", "2.0"}, {"method", method}};
    if (id >= 0) m["id"] = id;
    if (!params.is_null()) m["params"] = params;
    return mcp_write(s, m.dump() + "\n");
}

inline bool mcp_handle_message(McpServer& s, const json& message) {
    if (!message.is_object() || !message.contains("method")) return false;
    if (message["method"].is_string() &&
        message["method"] == "notifications/tools/list_changed")
        s.tools_changed = true;
    if (message.contains("id")) {
        json reply = {{"jsonrpc", "2.0"}, {"id", message["id"]}};
        if (message["method"].is_string() && message["method"] == "ping")
            reply["result"] = json::object();
        else
            reply["error"] = {{"code", -32601}, {"message", "method not found"}};
        mcp_write(s, reply.dump() + "\n");
    }
    return true;
}

// Consume notifications that arrived while no request was outstanding. This
// is called at model-request boundaries so an idle server can change its tool
// list without first receiving a tools/call.
inline void mcp_drain_inbound(McpServer& s) {
    while (s.alive) {
        struct pollfd descriptor = {s.out, POLLIN, 0};
        int ready = poll(&descriptor, 1, 0);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        if (descriptor.revents & (POLLIN | POLLHUP)) {
            char buffer[1 << 16];
            ssize_t count = read(s.out, buffer, sizeof buffer);
            if (count <= 0) {
                s.shutdown();
                break;
            }
            s.rbuf.append(buffer, static_cast<size_t>(count));
            if (!mcp_buffer_ok(s)) break;
        }
    }
    for (;;) {
        size_t newline = s.rbuf.find('\n');
        if (newline == std::string::npos) break;
        std::string line = s.rbuf.substr(0, newline);
        s.rbuf.erase(0, newline + 1);
        json message = json::parse(line, nullptr, false);
        // At this boundary no client request is outstanding. Non-request
        // messages are stale responses and can be discarded safely.
        if (!message.is_discarded()) mcp_handle_message(s, message);
    }
}

// wait for the response to `id`. Server pings are answered, other
// server->client requests get "method not found", notifications and stale
// responses (e.g. from an earlier cancelled call) are dropped.
inline json mcp_await(McpServer& s, long id, long timeout_s, bool cancellable) {
    auto fail = [&](const std::string& msg) { return json{{"error", {{"message", msg}}}}; };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    std::string line;
    while (mcp_read_line(s, line, deadline, cancellable)) {
        json m = json::parse(line, nullptr, false);
        if (m.is_discarded() || !m.is_object()) continue;
        if (mcp_handle_message(s, m)) continue;
        if (m.contains("id") && m["id"] == id) return m;
    }
    if (cancellable && abort_requested()) return fail("cancelled");
    if (!s.alive)
        return fail("server exited (stderr: " + mcp_log_path(s.name) + ")");
    return fail("no response after " + std::to_string(timeout_s) + "s");
}

inline json mcp_rpc(McpServer& s, const std::string& method, const json& params,
                    long timeout_s, bool cancellable = false) {
    long id = s.next_id++;
    if (!mcp_send(s, id, method, params))
        return {{"error", {{"message", "server not responding (stderr: " +
                                           mcp_log_path(s.name) + ")"}}}};
    json r = mcp_await(s, id, timeout_s, cancellable);
    if (cancellable && abort_requested()) {  // tell the server to actually stop the work
        clear_abort();  // mcp_write refuses while the abort flag is up
        mcp_send(s, -1, "notifications/cancelled",
                 {{"requestId", id}, {"reason", "user cancelled"}});
        request_abort();  // restore: the caller reports the cancellation
    }
    return r;
}

// --- results & schemas -------------------------------------------------------

// tools/call response -> bounded model-readable text. Text blocks remain
// natural text; every other standard or future content block is preserved as
// JSON instead of being silently replaced by a placeholder.
inline std::string mcp_result_text(const McpServer& s, const json& resp) {
    if (!resp.contains("result")) {
        std::string msg = "unknown error";
        if (resp.contains("error") && resp["error"].is_object())
            msg = resp["error"].value("message", msg);
        return "error: mcp(" + s.name + "): " + msg;
    }
    const json& r = resp["result"];
    if (!r.is_object())
        return "error: mcp(" + s.name + "): result must be an object";
    std::string text;
    if (r.contains("content") && r["content"].is_array())
        for (const json& c : r["content"]) {
            if (!text.empty()) text += '\n';
            std::string type;
            if (c.is_object() && c.contains("type") && c["type"].is_string())
                type = c["type"].get<std::string>();
            if (type == "text" && c.is_object() && c.contains("text") &&
                c["text"].is_string())
                text += c["text"].get<std::string>();
            else
                text += "[mcp " + (type.empty() ? "content" : type) + "]\n" +
                        c.dump();
        }
    if (r.contains("structuredContent")) {
        if (!text.empty()) text += '\n';
        text += "[mcp structuredContent]\n" + r["structuredContent"].dump();
    }
    if (text.empty()) text = "(empty result)";
    bool is_error = r.contains("isError") && r["isError"].is_boolean() &&
                    r["isError"].get<bool>();
    if (is_error && text.rfind("error", 0) != 0) text = "error: " + text;
    return text;
}

// cap without splitting a UTF-8 codepoint
inline std::string mcp_cap_desc(std::string d) {
    long cap = env_long("UAGENT_MCP_DESC_CHARS", 400);
    return cap > 0 ? utf8_trunc(std::move(d), (size_t)cap) : d;
}

// <server>_<tool>, restricted to the [A-Za-z0-9_-]{1,64} function-name charset
inline std::string mcp_tool_name(const std::string& server, const std::string& tool) {
    std::string n = server + "_" + tool;
    for (auto& c : n)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') c = '_';
    if (n.size() > 64) n.resize(64);
    return n;
}

// --- config & registration ---------------------------------------------------

inline bool mcp_validate_server_config(const std::string& name, const json& conf,
                                       std::string& error) {
    if (!conf.is_object()) {
        error = "server entry must be an object";
        return false;
    }
    auto require_type = [&](const char* field, json::value_t type,
                            const char* expected) {
        if (!conf.contains(field) || conf[field].type() == type) return true;
        error = std::string("`") + field + "` must be " + expected;
        return false;
    };
    if (!require_type("command", json::value_t::string, "a string") ||
        !require_type("type", json::value_t::string, "a string") ||
        !require_type("cwd", json::value_t::string, "a string") ||
        !require_type("args", json::value_t::array, "an array") ||
        !require_type("env", json::value_t::object, "an object") ||
        !require_type("tools", json::value_t::array, "an array") ||
        !require_type("trust", json::value_t::boolean, "a boolean") ||
        !require_type("disabled", json::value_t::boolean, "a boolean"))
        return false;
    if (conf.value("command", "").empty()) {
        error = "`command` is required and must not be empty";
        return false;
    }
    if (conf.contains("args"))
        for (const json& value : conf["args"])
            if (!value.is_string()) {
                error = "every `args` entry must be a string";
                return false;
            }
    if (conf.contains("env"))
        for (const auto& [key, value] : conf["env"].items())
            if (key.empty() || !value.is_string()) {
                error = "every `env` entry must have a nonempty key and string value";
                return false;
            }
    if (conf.contains("tools"))
        for (const json& value : conf["tools"])
            if (!value.is_string()) {
                error = "every `tools` allowlist entry must be a string";
                return false;
            }
    static const std::set<std::string> known = {
        "type", "command", "args", "env", "cwd", "tools", "trust",
        "disabled", "__uagent_config_dir", "__uagent_builtin", "__uagent_mode"};
    for (const auto& [field, ignored] : conf.items()) {
        (void)ignored;
        if (!known.count(field))
            mcp_note(name, "unknown config field `" + field + "` ignored");
    }
    return true;
}

// A trusted ./.mcp.json then ~/.mcp.json then built-ins — earlier layers win.
// The credential/config loader never imports a project .env.
inline json mcp_load_config(const json& trusted_project, size_t max_bytes) {
    auto read = [max_bytes](const std::string& path) -> json {
        std::error_code ec;
        uintmax_t bytes = std::filesystem::file_size(path, ec);
        if (!ec && max_bytes > 0 && bytes > max_bytes) {
            mcp_error(path, "configuration exceeds byte limit");
            return json::object();
        }
        std::ifstream f(path);
        if (!f) return json::object();
        json j = json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            printf("%smcp: %s is not valid JSON — ignored%s\n", RED(), path.c_str(), RST());
            return json::object();
        }
        return j.contains("mcpServers") && j["mcpServers"].is_object() ? j["mcpServers"]
                                                                       : json::object();
    };
    auto annotate = [](json servers, const std::string& config_dir) {
        if (!servers.is_object()) return json::object();
        for (auto& [name, conf] : servers.items()) {
            (void)name;
            if (conf.is_object()) conf["__uagent_config_dir"] = config_dir;
        }
        return servers;
    };
    json cfg =
        trusted_project.is_object() && trusted_project.contains("mcpServers")
            ? annotate(trusted_project["mcpServers"], canonical_cwd())
            : json::object();
    std::string home_dir = user_home();
    json home =
        home_dir.empty()
            ? json::object()
            : annotate(read(home_dir + "/.mcp.json"), home_dir);
    for (auto& [name, conf] : home.items())
        if (!cfg.contains(name)) cfg[name] = conf;
    if (!cfg.contains(CHROME_MCP_NAME) && env_str("UAGENT_CHROME_DEVTOOLS", "1") != "0" &&
        env_long("UAGENT_DEPTH", 0) == 0) {
        std::string mode = env_str("UAGENT_CHROME_MODE", "isolated");
        cfg[CHROME_MCP_NAME] = chrome_mcp_config(mode == "user" ? mode : "isolated");
    }
    return cfg;
}

inline bool mcp_fetch_tool_definitions(
    McpServer& s, const RuntimeConfig& config,
    std::chrono::steady_clock::time_point deadline, json& listed) {
    listed = json::array();
    std::string cursor;
    std::set<std::string> cursors;
    long pages = 0;
    do {
        if (++pages > config.mcp_pages ||
            (!cursor.empty() && !cursors.insert(cursor).second)) {
            mcp_error(s.name, "tools/list exceeded pagination limits");
            return false;
        }
        long remaining = std::chrono::duration_cast<std::chrono::seconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0) {
            mcp_error(s.name, "tools/list deadline exceeded");
            return false;
        }
        json params = cursor.empty() ? json::object() : json{{"cursor", cursor}};
        json resp = mcp_rpc(s, "tools/list", params, remaining);
        if (!resp.contains("result") || !resp["result"].is_object()) {
            std::string message = "failed";
            if (resp.contains("error") && resp["error"].is_object() &&
                resp["error"].contains("message") &&
                resp["error"]["message"].is_string())
                message = resp["error"]["message"].get<std::string>();
            mcp_error(s.name, "tools/list: " + message);
            return false;
        }
        json page = resp["result"].value("tools", json::array());
        if (!page.is_array()) {
            mcp_error(s.name, "tools/list returned a non-array `tools` value");
            return false;
        }
        for (const json& definition : page) {
            if (static_cast<long>(listed.size()) >= config.mcp_tools) {
                mcp_error(s.name, "tool count limit exceeded");
                return false;
            }
            listed.push_back(definition);
        }
        const json& result = resp["result"];
        if (result.contains("nextCursor") && !result["nextCursor"].is_string()) {
            mcp_error(s.name, "tools/list returned a non-string `nextCursor`");
            return false;
        }
        cursor = result.value("nextCursor", "");
    } while (!cursor.empty());
    return true;
}

// Build a complete replacement before touching the shared registry. A failed
// refresh therefore leaves every previously usable tool in place.
inline bool mcp_replace_server_tools(std::vector<Tool>& tools, McpServer& s,
                                     const RuntimeConfig& config,
                                     const json& listed) {
    const std::string provider = "mcp:" + s.name;
    std::set<std::string> occupied;
    for (const Tool& tool : tools)
        if (tool.provider != provider) occupied.insert(tool.name);

    std::set<std::string> only;
    if (s.config.contains("tools"))
        for (const json& name : s.config["tools"])
            only.insert(name.get<std::string>());
    const bool trust = s.config.value("trust", false);

    std::vector<Tool> replacement;
    size_t schema_bytes = 0;
    size_t max_schema_bytes = static_cast<size_t>(config.mcp_schema_bytes);
    for (const json& definition : listed) {
        if (!definition.is_object() || !definition.contains("name") ||
            !definition["name"].is_string()) {
            mcp_note(s.name, "invalid tool definition skipped");
            continue;
        }
        const std::string remote_name = definition["name"].get<std::string>();
        if (remote_name.empty() || (!only.empty() && !only.count(remote_name)))
            continue;

        std::string task_support;
        if (definition.contains("execution") && definition["execution"].is_object() &&
            definition["execution"].contains("taskSupport") &&
            definition["execution"]["taskSupport"].is_string())
            task_support =
                definition["execution"]["taskSupport"].get<std::string>();
        if (task_support == "required") {
            mcp_note(s.name, remote_name + " skipped (requires MCP tasks)");
            continue;
        }

        json input_schema =
            definition.value("inputSchema", json{{"type", "object"}});
        if (!input_schema.is_object()) {
            mcp_note(s.name, remote_name + " skipped (inputSchema is not an object)");
            continue;
        }

        std::string tool_name = mcp_tool_name(s.name, remote_name);
        if (tool_name.empty() || occupied.count(tool_name)) {
            mcp_note(s.name, "duplicate tool name " + tool_name + " skipped");
            continue;
        }
        occupied.insert(tool_name);
        std::string description;
        if (definition.contains("description") &&
            definition["description"].is_string())
            description = definition["description"].get<std::string>();
        if (description.empty() && definition.contains("title") &&
            definition["title"].is_string())
            description = definition["title"].get<std::string>();
        if (description.empty()) description = remote_name;
        McpServer* server = &s;
        long call_timeout = config.mcp_timeout_s;
        Tool tool = make_tool(
            std::move(tool_name), mcp_cap_desc(description), std::move(input_schema),
            [server, remote_name, call_timeout](
                const json& arguments, const ToolContext& context) -> std::string {
                if (!server->alive)
                    return "error: mcp server " + server->name +
                           " has exited (stderr: " + mcp_log_path(server->name) + ")";
                json response;
                if (run_cancellable([&] {
                        response = mcp_rpc(
                            *server, "tools/call",
                            {{"name", remote_name}, {"arguments", arguments}},
                            context.remaining_seconds(call_timeout), true);
                    }))
                    return "error: call cancelled by user";
                return mcp_result_text(*server, response);
            });
        if (definition.contains("outputSchema") &&
            definition["outputSchema"].is_object())
            tool.output_schema = definition["outputSchema"];
        tool.provider = provider;
        tool.timeout_s = config.mcp_timeout_s;

        size_t tool_schema_bytes =
            tool.description.size() + tool.parameters.dump().size() +
            (tool.output_schema.is_null() ? 0 : tool.output_schema.dump().size());
        if (schema_bytes + tool_schema_bytes > max_schema_bytes) {
            mcp_note(s.name, "remaining tools skipped (schema byte limit)");
            break;
        }

        bool read_only =
            definition.contains("annotations") &&
            definition["annotations"].is_object() &&
            definition["annotations"].contains("readOnlyHint") &&
            definition["annotations"]["readOnlyHint"].is_boolean() &&
            definition["annotations"]["readOnlyHint"].get<bool>();
        tool.mutating = !(trust && read_only);
        schema_bytes += tool_schema_bytes;
        replacement.push_back(std::move(tool));
    }

    tools.erase(std::remove_if(tools.begin(), tools.end(),
                               [&](const Tool& tool) {
                                   return tool.provider == provider;
                               }),
                tools.end());
    tools.insert(tools.end(), std::make_move_iterator(replacement.begin()),
                 std::make_move_iterator(replacement.end()));
    mcp_note(s.name, std::to_string(replacement.size()) + " of " +
                         std::to_string(listed.size()) + " tools (~" +
                         fmt_tokens(static_cast<long>(schema_bytes / 4)) +
                         " schema tokens/request)");
    return true;
}

inline bool mcp_load_server_tools(
    std::vector<Tool>& tools, McpServer& server, const RuntimeConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    json listed;
    return mcp_fetch_tool_definitions(server, config, deadline, listed) &&
           mcp_replace_server_tools(tools, server, config, listed);
}

// Apply notifications only between tool batches, when the agent holds no Tool
// pointers. Failed refreshes retain the prior registry and retry later.
inline bool mcp_refresh_tools(
    std::vector<Tool>& tools, McpRuntime& runtime, const RuntimeConfig& config,
    std::chrono::steady_clock::time_point turn_deadline =
        std::chrono::steady_clock::time_point::max()) {
    bool changed = false;
    for (const auto& owned : runtime.servers()) {
        McpServer& server = *owned;
        if (server.alive) mcp_drain_inbound(server);
        if (!server.alive || !server.tools_changed) continue;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(config.mcp_timeout_s);
        if (turn_deadline != std::chrono::steady_clock::time_point::max())
            deadline = std::min(deadline, turn_deadline);
        if (mcp_load_server_tools(tools, server, config, deadline)) {
            server.tools_changed = false;
            changed = true;
            mcp_note(server.name, "tool registry refreshed");
        }
    }
    return changed;
}

inline bool mcp_start_configured(McpServer& server, const RuntimeConfig& config,
                                 long& initialize_id, std::string& error) {
    const json& conf = server.config;
    std::vector<std::string> args;
    if (conf.contains("args"))
        for (const json& value : conf["args"])
            args.push_back(value.get<std::string>());
    std::vector<std::pair<std::string, std::string>> env;
    if (conf.contains("env"))
        for (const auto& [key, value] : conf["env"].items())
            env.emplace_back(key, expand_process_env(value.get<std::string>()));

    std::filesystem::path cwd = conf.value("cwd", "");
    if (!cwd.empty() && cwd.is_relative())
        cwd = std::filesystem::path(conf.value("__uagent_config_dir", "")) / cwd;
    if (!cwd.empty()) {
        std::error_code ec;
        cwd = std::filesystem::weakly_canonical(cwd, ec);
        if (ec || !std::filesystem::is_directory(cwd, ec)) {
            error = "invalid cwd `" + cwd.string() + "`";
            return false;
        }
    }
    if (!mcp_spawn(server, conf["command"].get<std::string>(), args, env, cwd.string(),
                   static_cast<size_t>(config.mcp_log_bytes))) {
        error = "failed to start";
        return false;
    }
    initialize_id = server.next_id++;
    if (!mcp_send(server, initialize_id, "initialize",
                  {{"protocolVersion", "2025-11-25"},
                   {"capabilities", json::object()},
                   {"clientInfo", {{"name", "uagent"}, {"version", UAGENT_VERSION}}}})) {
        error = "failed to initialize";
        server.shutdown();
        return false;
    }
    return true;
}

inline bool mcp_finish_initialize(McpServer& server, long initialize_id, long timeout,
                                  std::string& error) {
    json init = mcp_await(server, initialize_id, timeout, false);
    if (!init.contains("result")) {
        error = "handshake failed";
        if (init.contains("error") && init["error"].is_object() &&
            init["error"].contains("message") && init["error"]["message"].is_string())
            error = init["error"]["message"].get<std::string>();
        return false;
    }
    const json& result = init["result"];
    std::string protocol = result.value("protocolVersion", "");
    if (protocol != "2025-11-25" && protocol != "2025-06-18") {
        error = "unsupported protocol version `" + protocol + "`";
        return false;
    }
    if (!result.contains("capabilities") || !result["capabilities"].is_object() ||
        !result["capabilities"].contains("tools")) {
        error = "server did not negotiate tools capability";
        return false;
    }
    mcp_send(server, -1, "notifications/initialized", json::object());
    return true;
}

inline bool mcp_restart(McpServer& server, const json& next_config,
                        const RuntimeConfig& config, long timeout, std::string& error) {
    json previous = server.config;
    server.shutdown();
    server.config = next_config;
    long initialize_id = -1;
    if (mcp_start_configured(server, config, initialize_id, error) &&
        mcp_finish_initialize(server, initialize_id, timeout, error)) {
        server.tools_changed = true;
        return true;
    }

    server.shutdown();
    server.config = std::move(previous);
    std::string restore_error;
    if (mcp_start_configured(server, config, initialize_id, restore_error) &&
        mcp_finish_initialize(server, initialize_id, timeout, restore_error))
        return false;
    server.shutdown();
    error += "; previous session could not be restored: " + restore_error;
    return false;
}

inline void mcp_add_chrome_session_tool(std::vector<Tool>& tools, McpRuntime& runtime,
                                        const RuntimeConfig& config) {
    McpServer* chrome = nullptr;
    for (const auto& server : runtime.servers())
        if (server->config.value("__uagent_builtin", "") == "chrome-devtools") {
            chrome = server.get();
            break;
        }
    if (!chrome || !chrome->alive) return;

    Tool& tool = add_tool(
        tools,
        make_tool(
            "chrome_session",
            "Switch Chrome DevTools session. Use isolated for a fresh browser or user for "
            "existing login state; user mode requires chrome://inspect/#remote-debugging and "
            "attaches lazily on the next browser call.",
            {{"type", "object"},
             {"properties",
              {{"mode", {{"type", "string"},
                         {"enum", json::array({"isolated", "user"})}}}}},
             {"required", json::array({"mode"})},
             {"additionalProperties", false}},
            [chrome, &config](const json& args,
                              const ToolContext& context) -> std::string {
                std::string mode = args.value("mode", "");
                if (mode != "isolated" && mode != "user")
                    return "error: mode must be isolated or user";
                if (chrome->config.value("__uagent_mode", "isolated") == mode)
                    return "Chrome DevTools is already using the " + mode + " session";
                json next = chrome_mcp_config(mode);
                std::string error;
                if (!mcp_restart(*chrome, next, config,
                                 context.remaining_seconds(config.mcp_timeout_s), error))
                    return "error: could not switch Chrome DevTools: " + error;
                return mode == "user"
                           ? "User Chrome session selected. The approval prompt appears only "
                             "when the next browser tool interacts"
                           : "Chrome DevTools will launch a fresh isolated browser on the "
                             "next browser call";
            }));
    tool.mutating = true;
    tool.provider = "builtin:chrome";
    tool.summary = [](const json& args) { return args.value("mode", ""); };
}

// Spawn configured servers, handshake, and append one Tool per server tool.
// Spawns everything first and handshakes second, so slow server boots
// (npx downloads, node startup) overlap instead of adding up.
inline void mcp_register(std::vector<Tool>& tools, McpRuntime& runtime,
                         const RuntimeConfig& config,
                         const json& trusted_project = nullptr) {
    json cfg = mcp_load_config(trusted_project,
                               static_cast<size_t>(config.mcp_config_bytes));
    if (cfg.empty()) return;
    long timeout = config.mcp_timeout_s;

    struct Boot { McpServer* s; long init_id; };
    std::vector<Boot> boots;
    // config and server replies are untrusted JSON: a wrong type anywhere
    // must skip that server, never take the agent down
    long max_servers = config.mcp_servers;
    long spawned = 0;
    for (auto& [name, conf] : cfg.items()) try {
        if (spawned >= max_servers) {
            mcp_note(name, "skipped (server limit reached)");
            continue;
        }
        std::string config_error;
        if (!mcp_validate_server_config(name, conf, config_error)) {
            mcp_error(name, "invalid config: " + config_error);
            continue;
        }
        if (conf.value("disabled", false)) {
            mcp_note(name, "disabled");
            continue;
        }
        std::string type = conf.value("type", "stdio");
        if (type != "stdio") {
            mcp_note(name, "skipped (transport `" + type + "` not supported)");
            continue;
        }
        auto srv = std::make_unique<McpServer>();
        srv->name = name;
        srv->response_cap = static_cast<size_t>(config.mcp_response_bytes);
        srv->config = conf;
        long id = -1;
        std::string start_error;
        if (!mcp_start_configured(*srv, config, id, start_error)) {
            mcp_error(name, start_error);
            continue;
        }
        // queue the handshake now (the pipe buffers it); reap replies below
        boots.push_back({srv.get(), id});
        runtime.add(std::move(srv));
        ++spawned;
    } catch (const std::exception& e) {
        mcp_error(name, std::string("bad config: ") + e.what());
    }

    auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    for (auto& b : boots) try {
        McpServer& s = *b.s;
        long remaining = std::chrono::duration_cast<std::chrono::seconds>(
                             startup_deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0) {
            mcp_error(s.name, "startup deadline exceeded");
            s.shutdown();
            continue;
        }
        std::string initialize_error;
        if (!mcp_finish_initialize(s, b.init_id, remaining, initialize_error)) {
            mcp_error(s.name, initialize_error);
            s.shutdown();
            continue;
        }

        if (!mcp_load_server_tools(tools, s, config, startup_deadline)) {
            s.shutdown();
            continue;
        }
    } catch (const std::exception& e) {
        mcp_error(b.s->name, e.what());
        b.s->shutdown();
    }
    mcp_add_chrome_session_tool(tools, runtime, config);
}
