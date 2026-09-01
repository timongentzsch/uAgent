// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#include "include/core/fd.h"
#include "include/core/platform.h"
#include "include/core/time.h"

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

bool ReapPidFor(pid_t pid, int* status, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    pid_t result = WaitPid(pid, status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) return true;
    if (result < 0 || std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    int wait_ms = std::min(10, PollTimeoutMs(deadline));
    if (wait_ms <= 0) return false;
    (void)poll(nullptr, 0, wait_ms);
  }
}

std::string ProcessIdentity(pid_t pid) {
  if (pid <= 0) return {};
#ifdef __APPLE__
  proc_bsdinfo info{};
  int bytes = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info));
  if (bytes != static_cast<int>(sizeof(info))) return {};
  timeval boot{};
  size_t boot_size = sizeof(boot);
  if (sysctlbyname("kern.boottime", &boot, &boot_size, nullptr, 0) != 0 ||
      boot_size != sizeof(boot)) {
    return {};
  }
  return "macos:" + std::to_string(boot.tv_sec) + ":" +
         std::to_string(boot.tv_usec) + ":" +
         std::to_string(info.pbi_start_tvsec) + ":" +
         std::to_string(info.pbi_start_tvusec);
#elif defined(__linux__)
  std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
  std::string stat;
  if (!std::getline(input, stat)) return {};
  size_t command_end = stat.rfind(')');
  if (command_end == std::string::npos || command_end + 2 >= stat.size()) {
    return {};
  }
  std::istringstream fields(stat.substr(command_end + 2));
  std::string field;
  // Field 22 (starttime) is token 20 after the parenthesized command: the
  // first token here is field 3 (state).
  for (int index = 0; index <= 19; ++index) {
    if (!(fields >> field)) return {};
  }
  std::ifstream boot_input("/proc/sys/kernel/random/boot_id");
  std::string boot_id;
  if (!(boot_input >> boot_id) || boot_id.empty()) return {};
  return "linux:" + boot_id + ":" + field;
#else
  return {};
#endif
}

}  // namespace uagent
