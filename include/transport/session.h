// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_TRANSPORT_SESSION_H_
#define UAGENT_INCLUDE_TRANSPORT_SESSION_H_

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "include/core/env.h"
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
inline constexpr int kSocketBacklog = 16;
inline constexpr auto kConnectTimeout = std::chrono::seconds(2);
inline constexpr auto kConnectPollInterval = std::chrono::milliseconds(100);
inline constexpr auto kWorkerShutdownTimeout = std::chrono::seconds(5);
// A coordinator with nothing to do for this long is let go by the web host
// and exits once no client holds it; the next message or thread event starts
// it again. Tests shorten it.
inline std::chrono::seconds CoordinatorIdle() {
  return std::chrono::seconds(
      std::max<int64_t>(1, EnvLong("UAGENT_INTERNAL_COORDINATOR_IDLE_S", 600)));
}
inline constexpr auto kStreamBatchInterval =
    std::chrono::milliseconds(kStreamBatchIntervalMs);
inline constexpr auto kUsagePublishInterval =
    std::chrono::milliseconds(kUsageProgressIntervalMs);

// OS randomness for credentials and opaque identities. Empty on failure.
std::string RandomToken(size_t bytes = 24);
bool OpaqueId(std::string_view value);
// Addresses `frame` to session `id` at `generation` in this protocol.
void StampFrame(json& frame, const std::string& id,
                const std::string& generation);
bool WriteFrame(int fd, const json& frame);
bool WriteFrame(int fd, std::string line);
// The same incremental, bounded decoder serves worker commands and client
// events. Callback false terminates decoding at a complete frame.
class FrameBuffer {
 public:
  explicit FrameBuffer(size_t limit = kFrameBytes) : limit_(limit) {}
  bool Feed(std::string_view bytes, const std::function<bool(json)>& receive);

 private:
  size_t limit_;
  std::string pending_;
};
// Reads until `receive` declines, the peer closes, `stop_fd` turns readable
// or the deadline passes. A `stop_fd` of -1 never stops the read.
void ReadFrames(int fd, int stop_fd, size_t limit,
                const std::function<bool(json)>& receive,
                std::chrono::steady_clock::time_point deadline =
                    std::chrono::steady_clock::time_point::max(),
                bool interruptible = false);

struct Pipe {
  Fd read, write;
  bool Open();
  void Wake() const;
  void Drain() const;
};

}  // namespace uagent::session
#endif
