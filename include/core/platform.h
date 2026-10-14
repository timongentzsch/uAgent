// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PLATFORM_H_
#define UAGENT_INCLUDE_CORE_PLATFORM_H_

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace uagent {

// POSIX process environment. Kept behind one boundary so consumers do not
// redeclare the platform symbol.
char** ProcessEnvironment();

// Write every byte or report failure. errno is left set for the caller.
bool WriteAll(int fd, const void* data, size_t size);

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

// Whether process group `group` still holds anything that can run: its leader
// (a zombie one included, since whoever reaps it is still to come) or any
// member that is not a zombie. A group left with only the zombies of
// grandchildren is finished: their adopter reaps them whenever it gets to
// it, and nothing can be done about that from here.
bool ProcessGroupHasLiveMember(pid_t group);

// Stable identity for one live process, independent of PID reuse. Empty when
// the process does not exist or its platform metadata cannot be read.
std::string ProcessIdentity(pid_t pid);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PLATFORM_H_
