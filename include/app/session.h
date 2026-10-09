// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SESSION_H_
#define UAGENT_INCLUDE_APP_SESSION_H_
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "include/app/options.h"
#include "include/transport/session.h"
namespace uagent::session {
// State only checkpoints carry. Between them clients keep the last
// checkpoint's and apply block events to its view, so these large fields
// never cross the wire on every phase or usage change.
inline constexpr const char* kCheckpointFields[] = {"view", "http",
                                                    "system_prompt", "answer"};

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

std::string SocketPath(const std::string& path);
struct Connection {
  Fd socket;
  std::string generation;
  int pid = -1;
  // FileIdentity of the executable the worker started from, from its hello.
  std::string binary;
  // What the runtime said it was doing when it answered: "needs you",
  // "working" or "idle"; empty before its first state.
  std::string status;
};
Connection Connect(const std::string& path);
// Open hands a runtime its options in a private launch file; this reads them
// back, all but the session role, which the worker vets itself.
Options OptionsFromLaunch(const json& launch);
Connection Open(const std::string& executable, const std::string& cwd,
                const std::string& path, const std::string& title,
                const Options& options, std::string& error);
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
// `uagent coord -p`: one turn of the folder's coordinator runtime, printed as
// text or, with --json, as {answer, session_id, stop}.
int CoordinatorPromptMain(const Options& options);
}  // namespace uagent::session
#endif
