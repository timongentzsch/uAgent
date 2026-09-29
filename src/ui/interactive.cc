// Copyright 2026 Timon Gentzsch

#include "include/ui/interactive.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

#include "include/core/platform.h"
#include "include/core/signals.h"

namespace uagent {

InteractiveOutput::~InteractiveOutput() { Stop(); }

bool InteractiveOutput::Start() {
  int descriptors[2];
  if (pipe(descriptors) != 0) return false;
  Fd read_end(descriptors[0]);
  Fd write_end(descriptors[1]);
  Fd saved(dup(STDOUT_FILENO));
  if (!saved) return false;
  fcntl(saved.Get(), F_SETFD, FD_CLOEXEC);
  fcntl(read_end.Get(), F_SETFL, fcntl(read_end.Get(), F_GETFL) | O_NONBLOCK);
  if (dup2(write_end.Get(), STDOUT_FILENO) < 0) return false;
  saved_ = std::move(saved);
  read_ = std::move(read_end);
  // From here stdout is the pipe, so signal handlers must not write there.
  SetSignalTerminalFd(saved_.Get());
  // Buffered, deliberately: unbuffered turned every putchar of the streaming
  // markdown renderer into its own write(2) and left the renderer's byte/time
  // flush governor with nothing to govern. Buffering hands that governor the
  // real flush control. Line buffering rather than fully buffered, because
  // callers outside this layer print notices with plain printf and rely on
  // them reaching the pipe when the line ends; only the mid-line streaming
  // path (which flushes on its own budget) and Stop() need explicit flushes.
  static char buffer[64 * 1024];
  setvbuf(stdout, buffer, _IOLBF, sizeof buffer);
  return true;
}

void InteractiveOutput::Stop() {
  if (!saved_) return;
  fflush(stdout);
  SetSignalTerminalFd(STDOUT_FILENO);
  dup2(saved_.Get(), STDOUT_FILENO);
  saved_.Reset();
  read_.Reset();
}

InteractiveOutputUpdate InteractiveOutput::Read(bool finish) {
  if (finish) fflush(stdout);
  std::string bytes;
  char buffer[8192];
  for (;;) {
    ssize_t count = read(read_.Get(), buffer, sizeof buffer);
    if (count > 0) {
      bytes.append(buffer, static_cast<size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  return transcript_.Feed(bytes, finish);
}

void InteractiveOutput::Write(const std::string& text) const {
  (void)WriteAll(saved_.Get(), text.data(), text.size());
}

}  // namespace uagent
