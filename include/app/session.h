// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SESSION_H_
#define UAGENT_INCLUDE_APP_SESSION_H_
// Shared definitions of the session protocol's state: which fields only
// checkpoints carry and how activities are counted.

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "include/cli/options.h"
#include "include/core/env.h"
#include "include/transport/session.h"
namespace uagent::session {
// State only checkpoints carry; clients keep it from the last one.
inline constexpr const char* kCheckpointFields[] = {"view", "http",
                                                    "system_prompt", "answer"};

// A detached process outlives its session by design and never counts.
inline bool ActivityRuns(const json& activity) {
  const std::string state =
      JsonValue(activity, "status", JsonValue(activity, "state", ""));
  return !JsonValue(activity, "detached", false) &&
         (state == "running" || state == "starting" || state == "stopping" ||
          state == "finishing");
}
// Running commands the turn waits on, and those running behind it.
struct ActivityCounts {
  size_t background = 0, foreground = 0;
};
inline ActivityCounts CountActivities(const json& state) {
  ActivityCounts counts;
  const json* rows = JsonArray(state, "activities");
  if (!rows) return counts;
  for (const json& row : *rows) {
    if (JsonValue(row, "status", "") != "running") continue;
    if (JsonValue(row, "foreground", false)) {
      ++counts.foreground;
    } else {
      ++counts.background;
    }
  }
  return counts;
}
// An idle session stops its runtime after this long. Tests shorten it.
inline std::chrono::seconds IdlePeriod() {
  return std::chrono::seconds(std::max<int64_t>(
      1, EnvLong("UAGENT_INTERNAL_IDLE_S", IdleMinutes() * 60)));
}

// `state` without its checkpoint-only fields.
inline json LightState(const json& state) {
  json light = json::object();
  for (auto it = state.begin(); it != state.end(); ++it) {
    if (std::ranges::find(kCheckpointFields, it.key()) ==
        std::end(kCheckpointFields)) {
      light[it.key()] = it.value();
    }
  }
  return light;
}

inline std::string NewRequestId() { return RandomToken(16); }
inline json ControlFrame(const char* kind) {
  return {{"kind", kind}, {"request_id", NewRequestId()}};
}

std::string SocketPath(const std::string& path);
struct Connection {
  Fd socket;
  std::string generation;
  int pid = -1;
  // FileIdentity of the executable the worker started from, from its hello.
  std::string binary;
  // "needs you", "working" or "idle"; empty before its first state.
  std::string status;
};
Connection Connect(const std::string& path);
// Open hands a runtime its options in a private launch file; this reads them
// back, all but the session role, which the worker vets itself.
Options OptionsFromLaunch(const json& launch);
Connection Open(const std::string& executable, const std::string& cwd,
                const std::string& path, const std::string& title,
                const Options& options, std::string& error);
// Starts the saved session at `path`, or connects to the runtime it has.
Connection Wake(const std::string& cwd, const std::string& path,
                std::string& error);
// One poll thread owns all sockets. Publishing never blocks the model thread.
class Server {
 public:
  Server();
  ~Server();
  // Zero once it listens, or the WorkerExit that says why it does not.
  int Start(const std::string& path, const std::string& generation,
            std::function<bool(const json&)> command);
  void Publish(json frame);
  size_t Clients() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
int WorkerMain(int argc, char** argv);
int TerminalMain(Options options);
}  // namespace uagent::session
#endif
