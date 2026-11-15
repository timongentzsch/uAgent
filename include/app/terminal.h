// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_TERMINAL_H_
#define UAGENT_INCLUDE_APP_TERMINAL_H_
// The web host's and a remote gateway's terminals: the user's login shells
// on a PTY, unsandboxed. The agent never reads them.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "include/core/fd.h"
#include "include/core/thread_annotations.h"

namespace uagent {

// Unattached panel shells and their main-side records share this grace period.
inline constexpr std::chrono::seconds kTerminalFirstViewerWait{30};
struct TerminalShell;

// A shell as the page lists it, under the folder it belongs to.
struct TerminalInfo {
  std::string id{};
  std::string cwd{};
  std::string folder{};
  std::string title{};
  int64_t started = 0;  // wall-clock milliseconds
};

// The output past what the viewer has read; then, once, that the shell ended
// (`code` -1 when unknown) or that a newer viewer took it over.
struct TerminalOutput {
  std::string bytes{};
  bool ended = false;
  bool replaced = false;
  int code = -1;
};

// One device attached to a shell. Only the newest viewer types into it.
// Destruction detaches: a folder shell's idle time starts, any other ends.
class TerminalViewer {
 public:
  TerminalViewer(std::shared_ptr<TerminalShell> shell, uint64_t generation);
  ~TerminalViewer();
  TerminalViewer(const TerminalViewer&) = delete;
  TerminalViewer& operator=(const TerminalViewer&) = delete;

  bool Write(std::string_view input);
  bool Resize(uint16_t rows, uint16_t cols);
  // Waits up to `wait`; the first read replays what the shell kept.
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
  // At most `limit` shells at once. A folder's shell no device has attached
  // that printed nothing for `idle` ends. `changed` hears that the list did.
  Terminals(size_t limit, std::chrono::milliseconds idle,
            std::function<void()> changed = {});
  ~Terminals();
  Terminals(const Terminals&) = delete;
  Terminals& operator=(const Terminals&) = delete;

  // A shell in `directory` (empty: home) of `folder`; without a folder it
  // ends with its viewer and is never listed. Its id, or empty with `error`.
  std::string Open(const std::string& directory, const std::string& folder,
                   std::string& error);
  // Renames the shell `id`; empty on success, else why not.
  std::string Rename(const std::string& id, const std::string& title);
  // Ends the shell; false when there is no such shell.
  bool Close(const std::string& id);
  // The running folder shells, by folder, oldest first.
  std::vector<TerminalInfo> List() const;
  // A new viewer of `id`, replacing any earlier one; null for no such shell.
  std::unique_ptr<TerminalViewer> Attach(const std::string& id);
  // Ends every shell; Open refuses from then on.
  void Shutdown();
  bool Running() const;

 private:
  void Loop();

  const size_t limit_;
  const std::chrono::milliseconds idle_;
  const std::function<void()> changed_;
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

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_TERMINAL_H_
