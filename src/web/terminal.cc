// Copyright 2026 Timon Gentzsch

#include "include/web/terminal.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/child_env.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/signals.h"

namespace uagent::web {
namespace {
using Clock = std::chrono::steady_clock;

// A terminal starts at this size until its page says how large it is.
constexpr uint16_t kStartRows = 24;
constexpr uint16_t kStartColumns = 80;
// How long a write waits for a shell that reads nothing, and what a closed
// shell has to exit after its hangup before it is killed.
constexpr int kInputMs = 2000;
constexpr std::chrono::milliseconds kStopGrace{1000};
// What one read of a terminal takes; a wake reads at most the kept output,
// so a shell that never stops printing cannot hold up the others.
constexpr size_t kReadBytes = KiB(16);

int ExitCode(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

}  // namespace

struct TerminalShell {
  std::string id, cwd;
  pid_t pid = -1;
  // Read and closed only by the loop; written and resized under
  // `input_mutex`, which the loop takes to close it.
  Fd controller;
  std::mutex input_mutex;
  std::mutex mutex;
  std::condition_variable changed;
  // The newest output, at most kWebTerminalScrollbackBytes of it, and how
  // much the shell printed in all.
  std::string output UAGENT_GUARDED_BY(mutex);
  uint64_t produced UAGENT_GUARDED_BY(mutex) = 0;
  // The viewer that may type, 0 for none, and the last one given out.
  uint64_t viewer UAGENT_GUARDED_BY(mutex) = 0;
  uint64_t generations UAGENT_GUARDED_BY(mutex) = 0;
  // Its last output, or when its last viewer left.
  Clock::time_point quiet_since UAGENT_GUARDED_BY(mutex) = Clock::now();
  bool ended UAGENT_GUARDED_BY(mutex) = false;
  int code UAGENT_GUARDED_BY(mutex) = -1;
};

namespace {

// Moves what the shell printed into its kept output; false once its
// terminal hung up (every process holding it has gone).
bool Drain(TerminalShell& shell) {
  std::array<char, kReadBytes> buffer;
  for (size_t taken = 0; taken < kWebTerminalScrollbackBytes;) {
    const ssize_t count =
        read(shell.controller.Get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    const auto bytes = static_cast<size_t>(count);
    taken += bytes;
    std::lock_guard lock(shell.mutex);
    shell.output.append(buffer.data(), bytes);
    if (shell.output.size() > kWebTerminalScrollbackBytes) {
      shell.output.erase(0, shell.output.size() - kWebTerminalScrollbackBytes);
    }
    shell.produced += bytes;
    shell.quiet_since = Clock::now();
    shell.changed.notify_all();
  }
  return true;
}

// Hangs the terminal up, which sends the shell and its foreground job
// SIGHUP, so many shells ending together end at once.
void HangUp(TerminalShell& shell) {
  std::lock_guard lock(shell.input_mutex);
  shell.controller.Reset();
}

// Ends the shell: `status` when it was reaped already, else after a hangup,
// stopped and reaped here.
void Finish(TerminalShell& shell, std::optional<int> status) {
  {
    // Closed while it exited: its pid may already belong to another process.
    std::lock_guard lock(shell.mutex);
    if (shell.ended) return;
  }
  HangUp(shell);
  int wait_status = status.value_or(0);
  const bool known = status || StopProcess(shell.pid, /*group=*/true,
                                           kStopGrace, &wait_status);
  std::lock_guard lock(shell.mutex);
  shell.ended = true;
  shell.code = known ? ExitCode(wait_status) : -1;
  shell.changed.notify_all();
}

}  // namespace

TerminalViewer::TerminalViewer(std::shared_ptr<TerminalShell> shell,
                               uint64_t generation)
    : shell_(std::move(shell)), generation_(generation) {}

TerminalViewer::~TerminalViewer() {
  std::lock_guard lock(shell_->mutex);
  if (shell_->viewer == generation_) {
    shell_->viewer = 0;
    shell_->quiet_since = Clock::now();
  }
}

bool TerminalViewer::Write(std::string_view input) {
  std::lock_guard input_lock(shell_->input_mutex);
  {
    std::lock_guard lock(shell_->mutex);
    if (shell_->viewer != generation_ || shell_->ended) return false;
  }
  return shell_->controller &&
         WriteAllWithin(shell_->controller.Get(), input, kInputMs);
}

bool TerminalViewer::Resize(uint16_t rows, uint16_t cols) {
  std::lock_guard input_lock(shell_->input_mutex);
  {
    std::lock_guard lock(shell_->mutex);
    if (shell_->viewer != generation_ || shell_->ended) return false;
  }
  winsize size{};
  size.ws_row = rows;
  size.ws_col = cols;
  // The kernel tells the foreground job with SIGWINCH.
  return shell_->controller &&
         ioctl(shell_->controller.Get(), TIOCSWINSZ, &size) == 0;
}

TerminalOutput TerminalViewer::Read(std::chrono::milliseconds wait) {
  TerminalShell& shell = *shell_;
  UniqueLock lock(shell.mutex);
  shell.changed.wait_for(lock, wait, [&]() UAGENT_REQUIRES(shell.mutex) {
    return !replayed_ || woken_ || shell.viewer != generation_ ||
           shell.produced > cursor_ || shell.ended;
  });
  TerminalOutput out;
  if (woken_) return out;
  const uint64_t begin = shell.produced - shell.output.size();
  if (!replayed_) {
    replayed_ = true;
    cursor_ = begin;
    // Output cut short begins mid-line, perhaps mid-sequence: replay from
    // the first whole line.
    if (begin > 0) {
      const size_t line = shell.output.find('\n');
      if (line != std::string::npos) cursor_ += line + 1;
    }
  }
  // A viewer that fell behind what is kept goes on from the oldest output.
  cursor_ = std::max(cursor_, begin);
  const auto from = static_cast<size_t>(cursor_ - begin);
  out.bytes = shell.output.substr(from, kWebTerminalMessageBytes);
  cursor_ += out.bytes.size();
  if (cursor_ == shell.produced) {
    out.ended = shell.ended;
    out.code = shell.code;
  }
  out.replaced = !shell.ended && shell.viewer != generation_;
  return out;
}

void TerminalViewer::Wake() {
  std::lock_guard lock(shell_->mutex);
  woken_ = true;
  shell_->changed.notify_all();
}

Terminals::Terminals(size_t limit, std::chrono::milliseconds idle)
    : limit_(limit), idle_(idle) {
  int wake[2] = {-1, -1};
  if (OpenNonblockingPipe(wake)) {
    wake_read_.Reset(wake[0]);
    wake_write_.Reset(wake[1]);
  }
  // A shell that exits wakes the loop, even while something it started
  // still holds its terminal.
  InstallSigchldHandler();
  child_wake_registered_ = RegisterChildWakeFd(wake_write_.Get(), true);
  loop_ = std::thread([this] { Loop(); });
}

Terminals::~Terminals() {
  Shutdown();
  if (child_wake_registered_) {
    (void)RegisterChildWakeFd(wake_write_.Get(), false);
  }
}

std::string Terminals::Open(const std::string& directory, std::string& error) {
  const std::string folder = directory.empty() ? UserHome() : directory;
  const auto resolved = folder.starts_with('/')
                            ? CanonicalDirectory(folder)
                            : std::optional<std::filesystem::path>();
  if (!resolved) {
    error = "not a folder: " + folder;
    return {};
  }
  auto shell = std::make_shared<TerminalShell>();
  shell->id = RandomToken(16);
  shell->cwd = resolved->string();
  std::lock_guard lock(mutex_);
  if (stopping_ || shell->id.empty()) {
    error = "the terminal is unavailable";
    return {};
  }
  if (shells_.size() >= limit_) {
    error = "at most " + std::to_string(limit_) +
            " terminals run at once; close one first";
    return {};
  }
  std::string device;
  if (const int failed =
          OpenPty(shell->controller, device, kStartRows, kStartColumns);
      failed != 0 ||
      fcntl(shell->controller.Get(), F_SETFL,
            fcntl(shell->controller.Get(), F_GETFL) | O_NONBLOCK) != 0) {
    error = std::string("cannot open a terminal: ") +
            strerror(failed != 0 ? failed : errno);
    return {};
  }
  // The user's own shell, as a login shell, without uagent's credentials or
  // settings: "Add to chat" could carry them into a model's context. Their
  // ssh agent stays, so git push works as in any terminal.
  std::string program = EnvStr("SHELL");
  if (!program.starts_with('/')) program = "/bin/sh";
  ChildEnvironment environment({{"TERM", "xterm-256color"},
                                {"COLORTERM", "truecolor"},
                                {"PWD", shell->cwd}},
                               ChildEnvironmentPolicy::kUserShell);
  // posix_spawn has no chdir on every system this builds for, and this
  // process has threads, so sh enters the folder and becomes the shell.
  const SpawnSpec spec{
      .argv = {"/bin/sh", "-c", "cd -- \"$1\" && exec \"$0\" -l", program,
               shell->cwd},
      .environment = environment.Data(),
      .group = SpawnSpec::Group::kSession,
      .default_signals = {SIGINT, SIGTERM, SIGHUP, SIGPIPE},
      .fds = {{STDIN_FILENO, -1, device},
              {STDOUT_FILENO, STDIN_FILENO},
              {STDERR_FILENO, STDIN_FILENO}}};
  if (const int failed = SpawnProcess(spec, shell->pid); failed != 0) {
    error = std::string("cannot start a shell: ") + strerror(failed);
    return {};
  }
  shells_.push_back(shell);
  WakeDescriptor(wake_write_.Get());
  return shell->id;
}

bool Terminals::Close(const std::string& id) {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find(shells_, id, &TerminalShell::id);
  if (found == shells_.end()) return false;
  closing_.push_back(*found);
  shells_.erase(found);
  WakeDescriptor(wake_write_.Get());
  return true;
}

std::vector<TerminalInfo> Terminals::List() const {
  std::lock_guard lock(mutex_);
  std::vector<TerminalInfo> list;
  list.reserve(shells_.size());
  for (const auto& shell : shells_) list.push_back({shell->id, shell->cwd});
  return list;
}

std::unique_ptr<TerminalViewer> Terminals::Attach(const std::string& id) {
  std::shared_ptr<TerminalShell> shell;
  {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(shells_, id, &TerminalShell::id);
    if (found == shells_.end()) return nullptr;
    shell = *found;
  }
  uint64_t generation = 0;
  {
    std::lock_guard lock(shell->mutex);
    if (shell->ended) return nullptr;
    generation = ++shell->generations;
    shell->viewer = generation;
    // The viewer it replaces learns so.
    shell->changed.notify_all();
  }
  return std::make_unique<TerminalViewer>(std::move(shell), generation);
}

void Terminals::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  WakeDescriptor(wake_write_.Get());
  if (loop_.joinable()) loop_.join();
}

// The one owner of every terminal and shell process: reads their output,
// reaps them and ends those closed, exited or idle.
void Terminals::Loop() {
  for (;;) {
    std::vector<std::shared_ptr<TerminalShell>> live, ending;
    bool stopping = false;
    {
      std::lock_guard lock(mutex_);
      stopping = stopping_;
      live = shells_;
      ending.swap(closing_);
      if (stopping) {
        ending.insert(ending.end(), live.begin(), live.end());
        shells_.clear();
      }
    }
    if (stopping) {
      for (const auto& shell : ending) HangUp(*shell);
      for (const auto& shell : ending) Finish(*shell, std::nullopt);
      return;
    }
    for (const auto& shell : ending) Finish(*shell, std::nullopt);
    // Sleep until output, an exit, a wake or the first idle deadline; one a
    // viewer holds is looked at again an idle time from now.
    const auto now = Clock::now();
    auto deadline = now + idle_;
    std::vector<pollfd> ready{{wake_read_.Get(), POLLIN, 0}};
    for (const auto& shell : live) {
      ready.push_back({shell->controller.Get(), POLLIN, 0});
      std::lock_guard lock(shell->mutex);
      if (shell->viewer == 0) {
        deadline = std::min(deadline, shell->quiet_since + idle_);
      }
    }
    const int64_t wait = std::clamp<int64_t>(
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count(), 0,
        INT_MAX);
    PollRetry(ready.data(), static_cast<nfds_t>(ready.size()),
              static_cast<int>(wait));
    DrainDescriptor(wake_read_.Get());
    std::vector<std::shared_ptr<TerminalShell>> done;
    for (size_t at = 0; at < live.size(); ++at) {
      TerminalShell& shell = *live[at];
      int status = 0;
      const bool hung_up = ready[at + 1].revents != 0 && !Drain(shell);
      if (WaitPid(shell.pid, &status, WNOHANG) == shell.pid) {
        // What it printed last is still in the terminal.
        Drain(shell);
        Finish(shell, status);
      } else if (hung_up) {
        Finish(shell, std::nullopt);
      } else {
        bool idle = false;
        {
          std::lock_guard lock(shell.mutex);
          idle = shell.viewer == 0 && Clock::now() - shell.quiet_since >= idle_;
        }
        if (!idle) continue;
        Finish(shell, std::nullopt);
      }
      done.push_back(live[at]);
    }
    std::lock_guard lock(mutex_);
    for (const auto& shell : done) {
      std::erase(shells_, shell);
    }
  }
}

}  // namespace uagent::web
