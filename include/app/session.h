// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SESSION_H_
#define UAGENT_INCLUDE_APP_SESSION_H_
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "include/app/options.h"
#include "include/core/fd.h"
#include "include/core/json.h"
#include "include/core/limits.h"
namespace uagent::session {
inline constexpr int kProtocol = 2;
inline constexpr size_t kFrameBytes = MiB(1);
inline constexpr size_t kCommandBytes = KiB(512);
inline constexpr size_t kQueueBytes = MiB(4);
inline constexpr size_t kUploadBytes = MiB(8);
inline constexpr size_t kUploadCount = 8;
inline constexpr size_t kSessionAssetBytes = MiB(64);
inline constexpr size_t kGlobalAssetBytes = MiB(512);
inline constexpr size_t kReceiptEntries = 256;
inline constexpr size_t kMaxClients = 16;
inline constexpr size_t kMaxSessions = kMaxCatalogueEntries;
inline constexpr size_t kHelloBytes = KiB(1);
inline constexpr size_t kOutputCompactBytes = KiB(64);
inline constexpr size_t kBufferedNotices = 16;
inline constexpr size_t kIoBufferBytes = KiB(8);
inline constexpr size_t kWorkerIdentityBytes = 256;
inline constexpr int kSocketBacklog = 16;
inline constexpr auto kConnectTimeout = std::chrono::seconds(2);
inline constexpr auto kConnectPollInterval = std::chrono::milliseconds(100);
inline constexpr auto kWorkerShutdownTimeout = std::chrono::seconds(5);
inline constexpr auto kStreamBatchInterval =
    std::chrono::milliseconds(kStreamBatchIntervalMs);
inline constexpr auto kUsagePublishInterval =
    std::chrono::milliseconds(kUsageProgressIntervalMs);

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
// Identity of the executable a worker was spawned from (<mtime-ns>:<size>).
// Any rebuild, cp install, or mv swap bumps mtime; empty when unstated.
// Same-source rebuilds recycle once (harmless) rather than risk stale code.
std::string ExecutableIdentity(const std::string& executable);
// Sidecar recording which executable a session's worker was spawned from.
// Lives next to the worker socket; missing after /tmp clean or pre-feature.
std::string WorkerBinaryPath(const std::string& path);
bool WriteWorkerBinary(const std::string& path, const std::string& identity);
std::string ReadWorkerBinary(const std::string& path);
// Missing record (pre-feature spawn) or disagreement (upgraded binary)
// recycles; an unstatable executable never recycles blindly.
bool WorkerBinaryStale(const std::string& current, const std::string& recorded);
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
