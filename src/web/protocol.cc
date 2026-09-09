// Copyright 2026 Timon Gentzsch

#include "include/web/protocol.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <string>
#include <utility>

#include "include/core/platform.h"

namespace uagent::web {
std::string RandomToken(size_t bytes) {
  if (bytes > 64) {
    return {};
  }
  Fd fd(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
  std::array<unsigned char, 64> data{};
  size_t offset = 0;
  while (fd && offset < bytes) {
    ssize_t count = read(fd.Get(), data.data() + offset, bytes - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return {};
    }
    offset += static_cast<size_t>(count);
  }
  if (offset != bytes) {
    return {};
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    result += kHex[data[i] >> 4];
    result += kHex[data[i] & 15];
  }
  return result;
}

bool OpaqueId(std::string_view value) {
  return value.size() >= 16 && value.size() <= 64 &&
         value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

bool Pipe::Open() {
  int fds[2];
  if (!OpenNonblockingPipe(fds)) {
    return false;
  }
  read.Reset(fds[0]);
  write.Reset(fds[1]);
  return true;
}
void Pipe::Wake() const { WakeDescriptor(write.Get()); }
void Pipe::Drain() const { DrainDescriptor(read.Get()); }

bool WriteFrame(int fd, const json& frame) {
  return WriteFrame(fd, JsonDump(frame));
}

bool WriteFrame(int fd, std::string line) {
  if (line.size() > kFrameBytes) {
    return false;
  }
  line += '\n';
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  size_t offset = 0;
  while (offset < line.size()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    pollfd wait{fd, POLLOUT, 0};
    int ready = poll(&wait, 1, 100);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      return false;
    }
    if (ready == 0) {
      continue;
    }
    ssize_t count = write(fd, line.data() + offset, line.size() - offset);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  return true;
}

void ReadFrames(int fd, int stop_fd, size_t limit,
                const std::function<bool(json)>& receive) {
  std::string pending;
  char buffer[8192];
  for (;;) {
    pollfd waits[] = {{fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
    int ready = poll(waits, 2, -1);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0 || waits[1].revents != 0) {
      return;
    }
    ssize_t count = read(fd, buffer, sizeof buffer);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (count <= 0) {
      return;
    }
    pending.append(buffer, static_cast<size_t>(count));
    size_t begin = 0;
    for (;;) {
      size_t end = pending.find('\n', begin);
      if (end == std::string::npos) {
        break;
      }
      if (end - begin > limit) {
        return;
      }
      json frame = json::parse(
          pending.begin() + static_cast<std::ptrdiff_t>(begin),
          pending.begin() + static_cast<std::ptrdiff_t>(end), nullptr, false);
      if (!frame.is_object() || JsonValue(frame, "v", 0) != kProtocol ||
          !receive(std::move(frame))) {
        return;
      }
      begin = end + 1;
    }
    pending.erase(0, begin);
    if (pending.size() > limit) {
      return;
    }
  }
}
}  // namespace uagent::web
