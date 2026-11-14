// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PLATFORM_H_
#define UAGENT_INCLUDE_CORE_PLATFORM_H_
// The thin layer over POSIX that processes, pipes and descriptors go through.

#include <sys/types.h>
#include <sys/wait.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/fd.h"

namespace uagent {

// The process's `environ`, so no consumer redeclares the symbol.
char** ProcessEnvironment();

// Places the parent's `source`, else `path` opened read-write, else /dev/null
// at `target`; a source that is no target itself is closed in the child.
struct SpawnFd {
  int target = -1;
  int source = -1;
  std::string path = {};
};
struct SpawnSpec {
  std::vector<std::string> argv = {};
  // The child's environment; this process's when null.
  char* const* environment = nullptr;
  // A process group of its own (Ctrl+C at the terminal does not reach it),
  // or a session of its own (no controlling terminal either).
  enum class Group { kInherit, kOwn, kSession } group = Group::kInherit;
  // Signals the child starts with at their default action, and nothing
  // blocked; empty leaves both as inherited.
  std::vector<int> default_signals = {};
  std::vector<SpawnFd> fds = {};
};
// 0 with `pid` set, or the errno of the failed spawn.
int SpawnProcess(const SpawnSpec& spec, pid_t& pid);

// A pseudo-terminal of `rows` x `cols`: its close-on-exec controller and the
// path a child opens as its terminal. 0, or the errno with nothing left open.
int OpenPty(Fd& controller, std::string& terminal, uint16_t rows,
            uint16_t cols);

// Write every byte or report failure. errno is left set for the caller.
bool WriteAll(int fd, std::string_view data);

// The same, nonblocking: each chunk waits at most `timeout_ms` for the peer.
bool WriteAllWithin(int fd, std::string_view data, int timeout_ms);

// Self-pipe primitives. Wake writes coalesce and preserve errno.
bool OpenNonblockingPipe(int descriptors[2]);
void WakeDescriptor(int fd);
void DrainDescriptor(int fd);

// waitpid that retries EINTR. flags=0 blocks; WNOHANG polls.
pid_t WaitPid(pid_t pid, int* status, int flags = 0);
// Waits at most `timeout`. ECHILD is success: another owner reaped it.
bool ReapPidFor(pid_t pid, int* status, std::chrono::milliseconds timeout);
// SIGTERM a child (its group when `group`) and reap it within `grace`, else
// SIGKILL and reap within a second; zero grace kills at once. True if reaped.
bool StopProcess(pid_t pid, bool group, std::chrono::milliseconds grace,
                 int* status = nullptr);

inline bool ExitedOk(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// The fields used of a Linux /proc/<pid>/stat record.
struct ProcStat {
  char state;
  pid_t group;
};
std::optional<ProcStat> ParseProcStat(std::string_view record);

// Whether the /proc mount in `mountinfo` (/proc/self/mountinfo's text) lists
// every process: false for a hidepid other than 0/off, subset=pid or no mount.
bool ProcMountShowsEveryProcess(std::string_view mountinfo);

// Whether process group `group` can still run anything: false only when every
// member found is a single-task zombie whose leader is reaped.
bool ProcessGroupHasLiveMember(pid_t group);

// Identity of a live process that survives PID reuse; empty when unreadable.
std::string ProcessIdentity(pid_t pid);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PLATFORM_H_
