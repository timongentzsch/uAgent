// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_TERMINAL_H_
#define UAGENT_INCLUDE_WEB_TERMINAL_H_
// The web host's terminals: login shells of the user's on a PTY, unsandboxed,
// each keeping its recent output so a device can attach again after a reload.
// The agent never reads them.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "include/core/fd.h"
#include "include/core/thread_annotations.h"

namespace uagent::web {

struct TerminalShell;

// A shell as the page lists it.
struct TerminalInfo {
  std::string id{};
  std::string cwd{};
};

// What a viewer receives: the output past what it has read; then, once, that
// the shell ended (`code` its exit code, -1 when unknown) or that a newer
// viewer took the shell over.
struct TerminalOutput {
  std::string bytes{};
  bool ended = false;
  bool replaced = false;
  int code = -1;
};

// One device attached to a shell. Only the newest viewer of a shell types into
// it; an older one learns it was replaced. Destruction detaches, which starts
// the shell's idle time.
class TerminalViewer {
 public:
  TerminalViewer(std::shared_ptr<TerminalShell> shell, uint64_t generation);
  ~TerminalViewer();
  TerminalViewer(const TerminalViewer&) = delete;
  TerminalViewer& operator=(const TerminalViewer&) = delete;

  bool Write(std::string_view input);
  bool Resize(uint16_t rows, uint16_t cols);
  // Waits up to `wait` for output or an end; empty when neither came. The
  // first read replays what the shell kept.
  TerminalOutput Read(std::chrono::milliseconds wait);
  // Ends a Read waiting in another thread, now and for every later one.
  void Wake();

 private:
  std::shared_ptr<TerminalShell> shell_;
  const uint64_t generation_;
  uint64_t cursor_ = 0;
  bool replayed_ = false;
  std::atomic<bool> woken_{false};
};

class Terminals {
 public:
  // At most `limit` shells at once. One no device has attached that printed
  // nothing for `idle` ends.
  Terminals(size_t limit, std::chrono::milliseconds idle);
  ~Terminals();
  Terminals(const Terminals&) = delete;
  Terminals& operator=(const Terminals&) = delete;

  // A shell in `directory`, an absolute path to a folder (empty: the home
  // folder). Its id, or empty with `error`.
  std::string Open(const std::string& directory, std::string& error);
  // Ends the shell; false when there is no such shell.
  bool Close(const std::string& id);
  // The running shells, oldest first.
  std::vector<TerminalInfo> List() const;
  // A new viewer of the shell `id`, which replaces any earlier one; null when
  // there is no such shell.
  std::unique_ptr<TerminalViewer> Attach(const std::string& id);
  // Ends every shell; Open refuses from then on.
  void Shutdown();

 private:
  void Loop();

  const size_t limit_;
  const std::chrono::milliseconds idle_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<TerminalShell>> shells_ UAGENT_GUARDED_BY(mutex_);
  // Closed by a device, for the loop to end.
  std::vector<std::shared_ptr<TerminalShell>> closing_
      UAGENT_GUARDED_BY(mutex_);
  bool stopping_ UAGENT_GUARDED_BY(mutex_) = false;
  Fd wake_read_, wake_write_;
  bool child_wake_registered_ = false;
  std::thread loop_;
};

}  // namespace uagent::web

#endif  // UAGENT_INCLUDE_WEB_TERMINAL_H_
