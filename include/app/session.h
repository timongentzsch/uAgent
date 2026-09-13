// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SESSION_H_
#define UAGENT_INCLUDE_APP_SESSION_H_
#include <functional>
#include <memory>
#include <string>

#include "include/app/options.h"
#include "include/core/fd.h"
#include "include/core/json.h"
namespace uagent::session {
inline constexpr int kProtocol = 1;
inline constexpr size_t kFrameBytes = size_t{1024} * 1024;
inline constexpr size_t kCommandBytes = size_t{512} * 1024;
inline constexpr size_t kQueueBytes = size_t{4} * 1024 * 1024;
inline constexpr size_t kUploadBytes = size_t{8} * 1024 * 1024;
inline constexpr size_t kUploadCount = 8;
inline constexpr size_t kSessionAssetBytes = size_t{64} * 1024 * 1024;
inline constexpr size_t kGlobalAssetBytes = size_t{512} * 1024 * 1024;

// OS randomness for credentials and opaque identities. Empty on failure.
std::string RandomToken(size_t bytes = 24);
bool OpaqueId(std::string_view value);
bool WriteFrame(int fd, const json& frame);
bool WriteFrame(int fd, std::string line);
void ReadFrames(int fd, int stop_fd, size_t limit,
                const std::function<bool(json)>& receive);

struct Pipe {
  Fd read, write;
  bool Open();
  void Wake() const;
  void Drain() const;
};

std::string SocketPath(const std::string& path);
struct Connection {
  Fd socket;
  Fd owner;  // Delegated runtime lifetime; close when the parent session exits.
  std::string generation;
  int pid = -1;
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

 private:
  struct State;
  std::unique_ptr<State> state_;
};
int WorkerMain(int argc, char** argv);
int TerminalMain(Options options);
}  // namespace uagent::session
#endif
