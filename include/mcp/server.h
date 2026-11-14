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
#include <thread>
#include <utility>
#include <vector>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent {

// Stdio EOF is the protocol's polite stop; each signal follows after a grace.
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
  // in: the server's stdin, written here · out: its stdout, read here
  Fd in, out;
  bool alive = false;
  // Protocol 2026-07-28 is stateless: no initialize lifecycle is kept here.
  std::string rbuf;  // partial line from the server
  int64_t next_id = 1;
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
  // Why a configured server is not running; empty while it runs.
  std::string error;
  // Copies the server's stderr into its bounded log until the pipe closes.
  // Last member: replaced or destroyed, it stops and joins first.
  std::jthread log_thread;

  ~McpServer() { Shutdown(); }

  // Closing both ends is the polite stop signal for a stdio server.
  void CloseTransport();

  // Signals target the whole group (-pid): servers spawn their own workers.
  void Escalate(int signal_number);

  // True once the child is gone and dropped from the SIGINT kill list.
  bool Reaped();

  // Also called when a server is found dead or wedged, to close fds and reap.
  void Shutdown() { McpShutdown(*this); }
};

// Owns every session: destruction reaps every server before curl is torn down.
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

  // Swaps in `fresh` for `old`, which shuts down; an unknown `old` appends.
  McpServer& Replace(const McpServer& old, std::unique_ptr<McpServer> fresh) {
    for (auto& server : servers_) {
      if (server.get() == &old) {
        server = std::move(fresh);
        return *server;
      }
    }
    servers_.push_back(std::move(fresh));
    return *servers_.back();
  }

  // Closes every transport first so all see EOF together, then escalates in
  // lockstep: not a loop over one-server shutdowns.
  void ShutdownAll();

  // A control between turns changed the registry; the next refresh reports it.
  bool registry_changed = false;

 private:
  std::vector<std::unique_ptr<McpServer>> servers_;
};

// One voice for server status: notes are dim and bulleted, errors are red.
void McpNote(const std::string& name, const std::string& msg);
void McpError(const std::string& name, const std::string& msg);

std::string McpLogPath(const std::string& name);

// Where a failing server's own diagnostics went, appended to every error.
std::string McpStderrHint(const std::string& name);

// Appends one available chunk of stdout; false on close or the response cap.
bool McpFillBuffer(McpServer& s, bool eof_is_fatal = true);

bool McpTakeLine(McpServer& s, std::string& line);

// `wrapper` is the sandbox's argv before the command; empty runs unconfined.
bool McpSpawn(McpServer& s, const std::vector<std::string>& wrapper,
              const std::string& cmd, const std::vector<std::string>& args,
              const std::vector<std::pair<std::string, std::string>>& env,
              const std::string& cwd);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_SERVER_H_
