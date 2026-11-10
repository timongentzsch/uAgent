// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_SRC_APP_EXECUTION_INTERNAL_H_
#define UAGENT_SRC_APP_EXECUTION_INTERNAL_H_
#include <chrono>
#include <string>

#include "include/core/json.h"
#include "include/core/platform.h"
#include "include/transport/session.h"
namespace uagent::execution {
inline constexpr size_t kMessageBytes = MiB(128);
inline bool Send(int fd, const json& value) {
  fcntl(fd, F_SETFL, O_NONBLOCK);
  const std::string text = JsonDump(value);
  if (text.size() > kMessageBytes) return false;
  constexpr size_t chunk = KiB(128);
  for (size_t offset = 0; offset < text.size();) {
    size_t end = std::min(text.size(), offset + chunk);
    while (end < text.size() &&
           (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
      --end;
    if (!session::WriteFrame(fd,
                             json{{"v", session::kProtocol},
                                  {"data", text.substr(offset, end - offset)},
                                  {"end", end == text.size()}}))
      return false;
    offset = end;
  }
  return true;
}
inline json Receive(int fd, std::chrono::steady_clock::time_point deadline,
                    bool interruptible = false) {
  std::string text;
  bool complete = false;
  session::ReadFrames(
      fd, -1, session::kFrameBytes,
      [&](json frame) {
        const std::string data = JsonValue(frame, "data", "");
        if (text.size() + data.size() > kMessageBytes) return false;
        text += data;
        complete = JsonValue(frame, "end", false);
        return !complete;
      },
      deadline, interruptible);
  return complete ? json::parse(text, nullptr, false) : json();
}
}  // namespace uagent::execution
#endif
