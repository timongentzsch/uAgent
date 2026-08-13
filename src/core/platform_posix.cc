// Copyright 2026 Timon Gentzsch

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

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

pid_t WaitPid(pid_t pid, int* status, int flags) {
  pid_t result;
  do {
    result = waitpid(pid, status, flags);
  } while (result < 0 && errno == EINTR);
  return result;
}

}  // namespace uagent
