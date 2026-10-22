// Copyright 2026 Timon Gentzsch
#include "include/core/capture.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "include/core/fd.h"
#include "include/core/platform.h"
#include "include/core/time.h"
namespace uagent {
CapturedProcess CaptureProcess(const std::vector<std::string>& arguments,
                               int timeout_seconds, const std::string& body) {
  CapturedProcess result;
  if (arguments.empty()) {
    result.error = "empty helper command";
    return result;
  }
  int descriptors[2];
  if (!OpenNonblockingPipe(descriptors)) {
    result.error = "cannot open helper pipe";
    return result;
  }
  Fd input(descriptors[0]), output(descriptors[1]);
  int writing[2];
  if (!OpenNonblockingPipe(writing)) {
    result.error = "cannot open helper input";
    return result;
  }
  Fd source(writing[0]), sink(writing[1]);
  fcntl(source.Get(), F_SETFL, fcntl(source.Get(), F_GETFL) & ~O_NONBLOCK);
  // The child writes normally; only the parent reader is nonblocking.
  fcntl(output.Get(), F_SETFL, fcntl(output.Get(), F_GETFL) & ~O_NONBLOCK);
  pid_t child = -1;
  const int spawned = SpawnProcess({.argv = arguments,
                                    .group = SpawnSpec::Group::kOwn,
                                    .fds = {{STDOUT_FILENO, output.Get()},
                                            {STDERR_FILENO, output.Get()},
                                            {STDIN_FILENO, source.Get()}}},
                                   child);
  output.Reset();
  source.Reset();
  if (spawned != 0) {
    result.error = strerror(spawned);
    return result;
  }
  const auto deadline = DeadlineAfter(timeout_seconds);
  std::array<char, 8192> buffer;
  bool eof = false;
  size_t sent = 0;
  if (body.empty()) sink.Reset();
  while (!eof && PollTimeoutMs(deadline) > 0) {
    pollfd ready[]{{input.Get(), POLLIN, 0}, {sink.Get(), POLLOUT, 0}};
    int polled = poll(ready, 2, PollTimeoutMs(deadline));
    if (polled < 0 && errno == EINTR) continue;
    if (polled <= 0) break;
    if (sink && ready[1].revents) {
      ssize_t written =
          write(sink.Get(), body.data() + sent, body.size() - sent);
      if (written > 0) sent += static_cast<size_t>(written);
      if (sent == body.size() ||
          (written < 0 && errno != EINTR && errno != EAGAIN)) {
        sink.Reset();
      }
    }
    if (!ready[0].revents) continue;
    for (;;) {
      ssize_t count = read(input.Get(), buffer.data(), buffer.size());
      if (count == 0) {
        eof = true;
        break;
      }
      if (count < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN) eof = true;
        break;
      }
      if (result.output.size() + static_cast<size_t>(count) > MiB(1)) {
        result.error = "helper output exceeds limit";
        eof = true;
        break;
      }
      result.output.append(buffer.data(), static_cast<size_t>(count));
    }
  }
  int status = 0;
  if (!eof || !result.error.empty() ||
      !ReapPidFor(child, &status,
                  std::chrono::milliseconds(PollTimeoutMs(deadline)))) {
    StopProcess(child, true, {}, &status);
    if (result.error.empty()) result.error = "helper timed out";
  }
  result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}
}  // namespace uagent
