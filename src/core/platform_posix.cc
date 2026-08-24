// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>

#include "include/core/fd.h"
#include "include/core/platform.h"

extern char** environ;

namespace uagent {

char** ProcessEnvironment() { return environ; }

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const char*>(data);
  for (size_t offset = 0; offset < size;) {
    ssize_t written = write(fd, bytes + offset, size - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool OpenNonblockingPipe(int descriptors[2]) {
  if (pipe(descriptors) != 0) return false;
  // Owned until both ends are configured, so a half-configured pipe cannot
  // leak; the caller adopts them on the success path.
  Fd read_end(descriptors[0]);
  Fd write_end(descriptors[1]);
  for (int fd : {read_end.Get(), write_end.Get()}) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
      descriptors[0] = descriptors[1] = -1;
      return false;
    }
  }
  descriptors[0] = read_end.Release();
  descriptors[1] = write_end.Release();
  return true;
}

void WakeDescriptor(int fd) {
  if (fd < 0) return;
  const char byte = 1;
  int saved_errno = errno;
  ssize_t ignored = write(fd, &byte, 1);
  (void)ignored;
  errno = saved_errno;
}

void DrainDescriptor(int fd) {
  if (fd < 0) return;
  std::array<char, 128> bytes{};
  while (read(fd, bytes.data(), bytes.size()) > 0) {
  }
}

pid_t WaitPid(pid_t pid, int* status, int flags) {
  pid_t result;
  do {
    result = waitpid(pid, status, flags);
  } while (result < 0 && errno == EINTR);
  return result;
}

}  // namespace uagent
