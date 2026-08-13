// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PLATFORM_H_
#define UAGENT_INCLUDE_CORE_PLATFORM_H_

#include <sys/types.h>

#include <cstddef>

namespace uagent {

// POSIX process environment. Kept behind one boundary so consumers do not
// redeclare the platform symbol.
char** ProcessEnvironment();

// Write every byte or report failure. errno is left set for the caller.
bool WriteAll(int fd, const void* data, size_t size);

// waitpid that retries EINTR. flags=0 blocks; WNOHANG polls.
pid_t WaitPid(pid_t pid, int* status, int flags = 0);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PLATFORM_H_
