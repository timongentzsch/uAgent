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
    struct pollfd p[3] = {
        {s.in, POLLOUT, 0}, {s.out, POLLIN, 0}, {AbortWakeFd(), POLLIN, 0}};
    int pr = poll(p, 3, 10000);
    if (pr < 0 && errno == EINTR) continue;
    if (pr <= 0) {
      s.Shutdown();
      return false;
    }  // wedged server: kill it now
    if (p[2].revents & POLLIN) {
      if (AbortRequested()) return false;
      NormalizeAbortWake();
      continue;
    }
    if ((p[1].revents & POLLIN) && !McpFillBuffer(s, /*eof_is_fatal=*/false)) {
      return false;
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
    if (McpTakeLine(s, line)) return true;
    if (!s.alive) return false;
    if (cancellable && AbortRequested()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    struct pollfd p[2] = {{s.out, POLLIN, 0},
                          {cancellable ? AbortWakeFd() : -1, POLLIN, 0}};
    int pr = poll(p, 2, PollTimeoutMs(deadline));
    if (pr < 0 && errno != EINTR) {
      s.Shutdown();
      return false;
    }
    if (cancellable && (p[1].revents & POLLIN)) {
      if (AbortRequested()) return false;
      NormalizeAbortWake();
      continue;
    }
    // EOF here means the server exited: McpFillBuffer reaps it.
    if (pr > 0 && (p[0].revents & (POLLIN | POLLHUP)) && !McpFillBuffer(s)) {
      return false;
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
    reply["result"] = {{"roots", server.roots}};
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
    if ((descriptor.revents & (POLLIN | POLLHUP)) && !McpFillBuffer(s)) break;
  }
  std::string line;
  while (McpTakeLine(s, line)) {
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
inline json McpErrorReply(std::string message) {
  return json{{"error", {{"message", std::move(message)}}}};
}

inline json McpAwait(McpServer& s, int64_t id, int64_t timeout_s,
                     bool cancellable) {
  auto fail = [](std::string message) {
    return McpErrorReply(std::move(message));
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
  if (!s.alive) return fail("server exited" + McpStderrHint(s.name));
  return fail("no response after " + std::to_string(timeout_s) + "s");
}

inline json McpRpc(McpServer& s, const std::string& method, const json& params,
                   int64_t timeout_s, bool cancellable = false) {
  int64_t id = s.next_id++;
  if (!McpSend(s, id, method, params)) {
    return McpErrorReply("server not responding" + McpStderrHint(s.name));
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
