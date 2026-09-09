// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_WEB_PROTOCOL_H_
#define UAGENT_INCLUDE_WEB_PROTOCOL_H_

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent::web {
inline constexpr int kProtocol = 1;
inline constexpr size_t kFrameBytes = size_t{1024} * 1024;
inline constexpr size_t kCommandBytes = size_t{512} * 1024;
inline constexpr size_t kEventBytes = size_t{64} * 1024;
inline constexpr size_t kQueueBytes = size_t{4} * 1024 * 1024;
inline constexpr size_t kWorkerLimit = 4;
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

int WorkerMain(int argc, char** argv);
int MasterMain(const struct WebOptions& options, const char* executable);
struct WebOptions {
  int port = 8080;
  std::string origin;
  std::string push_contact;
};
}  // namespace uagent::web

#endif  // UAGENT_INCLUDE_WEB_PROTOCOL_H_
