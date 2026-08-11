// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_RPC_H_
#define UAGENT_INCLUDE_MCP_RPC_H_
// Newline-delimited JSON-RPC framing. Writes drain the server's stdout
// while blocked, so a chatty server can never deadlock a large write.

#include <poll.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/time.h"
#include "include/mcp/server.h"

namespace uagent {

// write a full message; drains the server's stdout while blocked so a chatty
// server can never deadlock a large write (both pipes full = classic hang)
inline bool McpWrite(McpServer& s, const std::string& data) {
  if (!s.alive) return false;
  size_t off = 0;
  while (off < data.size()) {
    if (AbortRequested()) return false;  // user hit Ctrl+C mid-call
    struct pollfd p[2] = {{s.in, POLLOUT, 0}, {s.out, POLLIN, 0}};
    int pr = poll(p, 2, 10000);
    if (pr < 0 && errno == EINTR) continue;
    if (pr <= 0) {
      s.Shutdown();
      return false;
    }  // wedged server: kill it now
    if (p[1].revents & POLLIN) {
      char buf[1 << 16];
      ssize_t n = read(s.out, buf, sizeof buf);
      if (n > 0) {
        s.rbuf.append(buf, static_cast<size_t>(n));
        if (!McpBufferOk(s)) return false;
      }
    }
    if (p[0].revents & (POLLERR | POLLHUP)) {
      s.Shutdown();
      return false;
    }
    if (p[0].revents & POLLOUT) {
      ssize_t n = write(s.in, data.data() + off, data.size() - off);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        s.Shutdown();
        return false;
      }
      off += static_cast<size_t>(n);
    }
  }
  return true;
}

// read one newline-terminated message; cancellable waits also stop on Ctrl+C
inline bool McpReadLine(McpServer& s, std::string& line,
                        std::chrono::steady_clock::time_point deadline,
                        bool cancellable) {
  for (;;) {
    size_t nl = s.rbuf.find('\n');
    if (nl != std::string::npos) {
      line = s.rbuf.substr(0, nl);
      s.rbuf.erase(0, nl + 1);
      return true;
    }
    if (!s.alive) return false;
    if (cancellable && AbortRequested()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    struct pollfd p = {s.out, POLLIN, 0};
    int pr = poll(&p, 1, 200);
    if (pr < 0 && errno != EINTR) {
      s.Shutdown();
      return false;
    }
    if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
      char buf[1 << 16];
      ssize_t n = read(s.out, buf, sizeof buf);
      if (n <= 0) {
        s.Shutdown();
        return false;
      }  // EOF: server exited — reap it
      s.rbuf.append(buf, static_cast<size_t>(n));
      if (!McpBufferOk(s)) return false;
    }
  }
}

// id >= 0: request · id < 0: notification
inline bool McpSend(McpServer& s, int64_t id, const std::string& method,
                    const json& params) {
  json m = {{"jsonrpc", "2.0"}, {"method", method}};
  if (id >= 0) m["id"] = id;
  if (!params.is_null()) m["params"] = params;
  return McpWrite(s, JsonDump(m) + "\n");
}

inline json McpClientRequestReply(const McpServer& server,
                                  const json& request) {
  json reply = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
  std::string method = JsonValue(request, "method", "");
  if (method == "ping") {
    reply["result"] = json::object();
  } else if (method == "roots/list") {
    reply["result"] = {{"roots", server.roots.entries}};
  } else {
    reply["error"] = {{"code", -32601}, {"message", "method not found"}};
  }
  return reply;
}

inline bool McpHandleMessage(McpServer& s, const json& message) {
  if (!message.is_object() || !message.contains("method")) return false;
  if (message["method"].is_string() &&
      message["method"] == "notifications/tools/list_changed") {
    s.tools_changed = true;
  }
  if (message.contains("id")) {
    json reply = McpClientRequestReply(s, message);
    McpWrite(s, JsonDump(reply) + "\n");
  }
  return true;
}

// Consume notifications that arrived while no request was outstanding. This
// is called at model-request boundaries so an idle server can change its tool
// list without first receiving a tools/call.
inline void McpDrainInbound(McpServer& s) {
  while (s.alive) {
    struct pollfd descriptor = {s.out, POLLIN, 0};
    int ready = poll(&descriptor, 1, 0);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) break;
    if (descriptor.revents & (POLLIN | POLLHUP)) {
      char buffer[1 << 16];
      ssize_t count = read(s.out, buffer, sizeof buffer);
      if (count <= 0) {
        s.Shutdown();
        break;
      }
      s.rbuf.append(buffer, static_cast<size_t>(count));
      if (!McpBufferOk(s)) break;
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
    if (!message.is_discarded()) McpHandleMessage(s, message);
  }
}

inline void McpDrainInbound(McpRuntime& runtime) {
  for (const auto& server : runtime.Servers()) {
    if (server->alive) McpDrainInbound(*server);
  }
}

// wait for the response to `id`. Server pings are answered, other
// server->client requests get "method not found", notifications and stale
// responses (e.g. from an earlier cancelled call) are dropped.
inline json McpAwait(McpServer& s, int64_t id, int64_t timeout_s,
                     bool cancellable) {
  auto fail = [&](const std::string& msg) {
    return json{{"error", {{"message", msg}}}};
  };
  auto deadline = DeadlineAfter(timeout_s);
  std::string line;
  while (McpReadLine(s, line, deadline, cancellable)) {
    json m = json::parse(line, nullptr, false);
    if (m.is_discarded() || !m.is_object()) continue;
    if (McpHandleMessage(s, m)) continue;
    if (m.contains("id") && m["id"] == id) return m;
  }
  if (cancellable && AbortRequested()) return fail("cancelled");
  if (!s.alive) {
    return fail("server exited (stderr: " + McpLogPath(s.name) + ")");
  }
  return fail("no response after " + std::to_string(timeout_s) + "s");
}

inline json McpRpc(McpServer& s, const std::string& method, const json& params,
                   int64_t timeout_s, bool cancellable = false) {
  int64_t id = s.next_id++;
  if (!McpSend(s, id, method, params)) {
    return {{"error",
             {{"message",
               "server not responding (stderr: " + McpLogPath(s.name) + ")"}}}};
  }
  json r = McpAwait(s, id, timeout_s, cancellable);
  if (cancellable &&
      AbortRequested()) {  // tell the server to actually stop the work
    ClearAbort();          // mcp_write refuses while the abort flag is up
    McpSend(s, -1, "notifications/cancelled",
            {{"requestId", id}, {"reason", "user cancelled"}});
    RequestAbort();  // restore: the caller reports the cancellation
  }
  return r;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_RPC_H_
