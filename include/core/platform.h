// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PLATFORM_H_
#define UAGENT_INCLUDE_CORE_PLATFORM_H_
// The thin layer over POSIX that processes, pipes and descriptors go
// through, so no caller declares or wraps the system calls itself.

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

// POSIX process environment. Kept behind one boundary so consumers do not
// redeclare the platform symbol.
char** ProcessEnvironment();

// One child process, started with posix_spawnp. Each `fds` entry places a
// descriptor at `target` in the child: the parent's `source`, else `path`
// opened read-write, else /dev/null. A source that is not itself a target is
// closed in the child once placed.
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

// A pseudo-terminal of `rows` x `cols`: its close-on-exec controller, and the
// path a child opens as its terminal (a SpawnFd `path`). 0, or the errno with
// nothing left open.
int OpenPty(Fd& controller, std::string& terminal, uint16_t rows,
            uint16_t cols);

// Write every byte or report failure. errno is left set for the caller.
bool WriteAll(int fd, std::string_view data);

// The same for a nonblocking descriptor: each chunk waits at most
// `timeout_ms` for the peer to drain, so a stuck reader cannot hang us.
bool WriteAllWithin(int fd, std::string_view data, int timeout_ms);

// Small poll/self-pipe primitives shared by signals, steering, activities and
// the interactive broker. Wake writes are coalescing and preserve errno.
bool OpenNonblockingPipe(int descriptors[2]);
void WakeDescriptor(int fd);
void DrainDescriptor(int fd);

// waitpid that retries EINTR. flags=0 blocks; WNOHANG polls.
pid_t WaitPid(pid_t pid, int* status, int flags = 0);
// Poll for one child without ever turning process teardown into an unbounded
// wait. ECHILD is success because another owner already reaped it.
bool ReapPidFor(pid_t pid, int* status, std::chrono::milliseconds timeout);
// SIGTERM a child (its group when `group`) and reap it within `grace`, else
// SIGKILL and reap within a second; zero grace kills at once. True if reaped.
bool StopProcess(pid_t pid, bool group, std::chrono::milliseconds grace,
                 int* status = nullptr);

// A wait status that says the process exited, and with code 0.
inline bool ExitedOk(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// The fields of a Linux /proc/<pid>/stat record that matter here. Nothing
// when the record is malformed.
struct ProcStat {
  char state;
  pid_t group;
};
std::optional<ProcStat> ParseProcStat(std::string_view record);

// Whether the /proc mount described by `mountinfo` (the text of
// /proc/self/mountinfo) lists every process as it is: false for a hidepid
// other than 0/off, for subset=pid, and when no /proc mount is found.
bool ProcMountShowsEveryProcess(std::string_view mountinfo);

// Whether process group `group`, known to answer a signal, still holds
// anything that can run. False only when the scan positively finds members and
// every one is a zombie with a single task, the leader already reaped: their
// adopter reaps them whenever it gets to it, and nothing can be done about
// that from here. Any doubt answers true.
bool ProcessGroupHasLiveMember(pid_t group);

// Stable identity for one live process, independent of PID reuse. Empty when
// the process does not exist or its platform metadata cannot be read.
std::string ProcessIdentity(pid_t pid);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PLATFORM_H_
