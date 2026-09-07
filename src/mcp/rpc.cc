// Copyright 2026 Timon Gentzsch

#include "include/mcp/rpc.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/time.h"
#include "include/mcp/server.h"

namespace uagent {

json McpRequestMeta() {
  return {
      {"io.modelcontextprotocol/protocolVersion", kMcpModernProtocolVersion},
      {"io.modelcontextprotocol/clientInfo",
       {{"name", "uagent"}, {"version", kVersion}}},
      {"io.modelcontextprotocol/clientCapabilities",
       {{"roots", json::object()}}}};
}

bool McpWrite(McpServer& s, const std::string& data) {
  if (!s.alive) return false;
  size_t off = 0;
  while (off < data.size()) {
    if (AbortRequested()) return false;  // user hit Ctrl+C mid-call
    struct pollfd p[3] = {{s.in.Get(), POLLOUT, 0},
                          {s.out.Get(), POLLIN, 0},
                          {AbortWakeFd(), POLLIN, 0}};
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
      ssize_t n = write(s.in.Get(), data.data() + off, data.size() - off);
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

bool McpReadLine(McpServer& s, std::string& line,
                 std::chrono::steady_clock::time_point deadline,
                 bool cancellable) {
  for (;;) {
    if (McpTakeLine(s, line)) return true;
    if (!s.alive) return false;
    if (cancellable && AbortRequested()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    struct pollfd p[2] = {{s.out.Get(), POLLIN, 0},
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

bool McpSend(McpServer& s, int64_t id, const std::string& method,
             const json& params) {
  json m = {{"jsonrpc", "2.0"}, {"method", method}};
  if (id >= 0) m["id"] = id;
  if (!params.is_null()) m["params"] = params;
  if (id >= 0) {
    if (!m.contains("params") || !m["params"].is_object()) {
      m["params"] = json::object();
    }
    m["params"]["_meta"] = McpRequestMeta();
  }
  return McpWrite(s, JsonDump(m) + "\n");
}

bool McpHandleMessage(McpServer& s, const json& message) {
  if (!message.is_object()) return false;
  if (s.subscription_id >= 0 && message.contains("id") &&
      message["id"] == s.subscription_id) {
    s.tools_subscribed = false;
    return true;
  }
  if (!message.contains("method")) return false;
  const json params = JsonValue(message, "params", json::object());
  const json meta = JsonValue(params, "_meta", json::object());
  if (!message.contains("id") && s.subscription_id >= 0) {
    std::string method = JsonValue(message, "method", "");
    if (method == "notifications/cancelled" &&
        JsonValue(params, "requestId", json(nullptr)) == s.subscription_id) {
      s.tools_subscribed = false;
    } else if (JsonValue(meta, "io.modelcontextprotocol/subscriptionId",
                         json(nullptr)) == s.subscription_id) {
      if (method == "notifications/subscriptions/acknowledged") {
        s.tools_subscribed =
            JsonValue(JsonValue(params, "notifications", json::object()),
                      "toolsListChanged", false);
      } else if (s.tools_subscribed &&
                 method == "notifications/tools/list_changed") {
        s.tools_changed = true;
      }
    }
  }
  // Modern stdio forbids server requests and client JSON-RPC responses.
  // Roots are answered only through input_required tool results.
  return true;
}

void McpFillAvailable(McpServer& s) {
  while (s.alive) {
    struct pollfd descriptor = {s.out.Get(), POLLIN, 0};
    int ready = poll(&descriptor, 1, 0);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) break;
    if ((descriptor.revents & (POLLIN | POLLHUP)) && !McpFillBuffer(s)) break;
  }
}

void McpDrainInbound(McpServer& s) {
  McpFillAvailable(s);
  std::string line;
  while (McpTakeLine(s, line)) {
    json message = json::parse(line, nullptr, false);
    // At this boundary no client request is outstanding. Non-request
    // messages are stale responses and can be discarded safely.
    if (!message.is_discarded()) McpHandleMessage(s, message);
  }
}

void McpDrainInbound(McpRuntime& runtime) {
  for (const auto& server : runtime.Servers()) {
    if (server->alive && server->startup == McpStartupState::kReady) {
      McpDrainInbound(*server);
    }
  }
}

json McpErrorReply(std::string message) {
  return json{{"error", {{"message", std::move(message)}}}};
}

bool McpResponseMatches(const json& message, int64_t id) {
  return JsonValue(message, "jsonrpc", "") == "2.0" &&
         JsonValue(message, "id", json(nullptr)) == id &&
         (message.contains("result") != message.contains("error")) &&
         (!message.contains("error") || message["error"].is_object());
}

McpResponseState McpTryResponse(McpServer& s, int64_t id, json& response) {
  McpFillAvailable(s);
  std::string line;
  while (McpTakeLine(s, line)) {
    json message = json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object()) continue;
    if (McpHandleMessage(s, message)) continue;
    if (McpResponseMatches(message, id)) {
      response = std::move(message);
      return McpResponseState::kReady;
    }
  }
  return s.alive ? McpResponseState::kPending : McpResponseState::kClosed;
}

json McpAwait(McpServer& s, int64_t id, int64_t timeout_s, bool cancellable) {
  auto fail = [](std::string message) {
    return McpErrorReply(std::move(message));
  };
  auto deadline = DeadlineAfter(timeout_s);
  std::string line;
  while (McpReadLine(s, line, deadline, cancellable)) {
    json m = json::parse(line, nullptr, false);
    if (m.is_discarded() || !m.is_object()) continue;
    if (McpHandleMessage(s, m)) continue;
    if (McpResponseMatches(m, id)) return m;
  }
  if (!s.alive) return fail("server exited" + McpStderrHint(s.name));
  bool cancelled = cancellable && AbortRequested();
  if (cancelled) ClearAbort();
  McpSend(s, -1, "notifications/cancelled",
          {{"requestId", id},
           {"reason", cancelled ? "user cancelled" : "request timed out"}});
  if (cancelled) {
    RequestAbort();
    return fail("cancelled");
  }
  return fail("no response after " + std::to_string(timeout_s) + "s");
}

json McpRpc(McpServer& s, const std::string& method, const json& params,
            int64_t timeout_s, bool cancellable) {
  int64_t id = s.next_id++;
  if (!McpSend(s, id, method, params)) {
    return McpErrorReply("server not responding" + McpStderrHint(s.name));
  }
  return McpAwait(s, id, timeout_s, cancellable);
}

bool McpValidateDiscovery(const json& discovery, std::string& error) {
  if (!discovery.contains("result") || !discovery["result"].is_object()) {
    error = JsonErrorMessage(discovery, "server discovery failed");
    return false;
  }
  const json& result = discovery["result"];
  json versions = JsonValue(result, "supportedVersions", json::array());
  bool supported = versions.is_array() &&
                   std::find(versions.begin(), versions.end(),
                             kMcpModernProtocolVersion) != versions.end();
  if (!supported) {
    error = "server does not support required protocol `" +
            std::string(kMcpModernProtocolVersion) + "`";
    return false;
  }
  if (!result.contains("capabilities") || !result["capabilities"].is_object() ||
      !JsonObject(result["capabilities"], "tools")) {
    error = "server did not negotiate tools capability";
    return false;
  }
  return true;
}

}  // namespace uagent
