// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_SERVER_H_
#define UAGENT_INCLUDE_MCP_SERVER_H_
// The MCP server process: its buffers, its spawn, and the runtime that
// owns every session so transports close before curl is torn down.

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent {

// Give stdio EOF a brief chance to perform the protocol's polite shutdown
// before escalating to signals, then again before the kill.
inline constexpr auto kMcpEofGrace = std::chrono::milliseconds(200);
inline constexpr auto kMcpTermGrace = std::chrono::milliseconds(300);
inline constexpr auto kMcpKillGrace = std::chrono::milliseconds(500);

enum class McpStartupState : uint8_t {
  kReady,
  kDiscovering,
  kDiscoveringTools,
};

struct McpServer;
void McpShutdown(McpServer& server);

struct McpServer {
  std::string name;
  pid_t pid = -1;
  // in: we write (the server's stdin) · out: we read (its stdout)
  Fd in, out;
  bool alive = false;
  // 2026-07-28 is stateless at the protocol layer. µAgent intentionally has
  // no legacy initialize lifecycle or downgrade state.
  std::string rbuf;  // partial line from the server
  int64_t next_id = 1;
  size_t response_cap = size_t{16} * 1024 * 1024;
  json config;
  json roots = json::array();
  bool tools_changed = false;
  int64_t subscription_id = -1;
  bool tools_subscribed = false;
  McpStartupState startup = McpStartupState::kReady;
  int64_t discovery_id = -1;
  int64_t tools_list_id = -1;
  int64_t startup_pages = 0;
  json startup_tools = json::array();
  std::string startup_cursor;
  std::set<std::string> startup_cursors;

  ~McpServer() { Shutdown(); }

  // Closing our ends is the polite stop signal for a stdio server.
  void CloseTransport();

  // Signals target the whole group (-pid): servers spawn their own workers.
  void Escalate(int signal_number);

  // True once the child is gone and dropped from the SIGINT kill list.
  bool Reaped();

  // Also called the moment a server is detected dead/wedged, so fds close and
  // the child is reaped immediately, not at program exit.
  void Shutdown() { McpShutdown(*this); }
};

// One shutdown ladder for one or many servers: close every transport first so
// they all observe EOF together, then escalate in lockstep. Batching is the
// reason ShutdownAll cannot simply loop over one-server shutdowns.

// Explicit session owner. Destruction closes every transport and reaps every
// server before curl and the rest of the process runtime are torn down.
class McpRuntime {
 public:
  McpRuntime() = default;
  ~McpRuntime() { ShutdownAll(); }
  McpRuntime(const McpRuntime&) = delete;
  McpRuntime& operator=(const McpRuntime&) = delete;

  void Add(std::unique_ptr<McpServer> server) {
    servers_.push_back(std::move(server));
  }

  const std::vector<std::unique_ptr<McpServer>>& Servers() const {
    return servers_;
  }

  void ShutdownAll();

 private:
  std::vector<std::unique_ptr<McpServer>> servers_;
};

// One voice for server status: notes are dim and bulleted, errors are red.
void McpNote(const std::string& name, const std::string& msg);
void McpError(const std::string& name, const std::string& msg);

std::string McpLogPath(const std::string& name);

// Where a failing server's own diagnostics went. Appended to every error the
// model or user sees, so the next step is always obvious.
std::string McpStderrHint(const std::string& name);

bool McpBufferOk(McpServer& s);

// Append one available chunk of server stdout to the read buffer. Returns
// false when the server closed or the response cap was hit. McpWrite's
// opportunistic drain tolerates EOF, so it passes eof_is_fatal=false.
bool McpFillBuffer(McpServer& s, bool eof_is_fatal = true);

// Take one complete newline-terminated message out of the read buffer.
bool McpTakeLine(McpServer& s, std::string& line);

bool McpSpawn(McpServer& s, const std::string& cmd,
              const std::vector<std::string>& args,
              const std::vector<std::pair<std::string, std::string>>& env,
              const std::string& cwd, size_t log_bytes);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_SERVER_H_
