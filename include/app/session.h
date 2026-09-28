// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SESSION_H_
#define UAGENT_INCLUDE_APP_SESSION_H_
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "include/app/options.h"
#include "include/transport/session.h"
namespace uagent::session {
std::string SocketPath(const std::string& path);
struct Connection {
  Fd socket;
  std::string generation;
  int pid = -1;
  // FileIdentity of the executable the worker started from, from its hello.
  std::string binary;
};
Connection Connect(const std::string& path);
Connection Open(const std::string& executable, const std::string& cwd,
                const std::string& path, const std::string& title,
                const Options& options, std::string& error);
// One poll thread owns all sockets. Publishing never blocks the model thread.
class Server {
 public:
  Server();
  ~Server();
  bool Start(const std::string& path, const std::string& generation,
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
