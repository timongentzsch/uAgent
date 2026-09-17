// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FS_H_
#define UAGENT_INCLUDE_CORE_FS_H_
// Agent directories and crash-safe file writing. Global state stays under
// ~/.uagent; explicitly project-scoped files use the workspace's .uagent.
// Bodies live in src/core/fs.cc so widely-included helpers do not recompile
// in every translation unit.

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <istream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/strings.h"

namespace uagent {

// The throwing overload aborts under -fno-exceptions on a lookup error
// (EACCES on a parent, ELOOP). A path we cannot look up is not there.
bool PathExists(const std::filesystem::path& path);

std::string UserHome();

std::string Tilde(const std::string& path);

// ~/.uagent. Falls back to a per-uid temp directory when the account has no
// home, never to a project-controlled location.
std::string GlobalBase();

// The directory a workspace opts into. Named once: several modules need it,
// and a reader and a writer disagreeing about it would silently lose data.
std::filesystem::path ProjectBase(const std::filesystem::path& cwd);

// Create a directory inside the private user state tree without following
// directory symlinks, and verify its ownership and mode.
bool EnsurePrivateDirectory(const std::string& path);

// The agent's directory layout. Named where a second file also spells the
// name: a writer, a reader and the pruner disagreeing would strand data
// somewhere nothing looks. Single-use names stay literal at their one site.
inline constexpr const char* kMemoryDir = "memory";
inline constexpr const char* kHistoryDir = "history";
inline constexpr const char* kSessionsDir = "sessions";
inline constexpr const char* kBgDir = "bg";
inline constexpr const char* kTerminalsDir = "terminals";
// Detached logs sit one level below their records rather than beside them. A
// record names the log to remove and the process group to kill, so a writer
// that can reach the records directory can forge one and redirect both. The
// logs are the only part a command's own output has to reach, so they are the
// only part that can be made writable. Readers need no split: every one of
// them follows the full path stored in the record, and the record scan is
// non-recursive and filters .json, so it does not see this subdirectory.
inline constexpr const char* kTerminalLogsDir = "terminals/logs";
inline constexpr const char* kArtifactsDir = "artifacts";
inline constexpr const char* kMcpDir = "mcp";
inline constexpr const char* kConfigDir = "config";

// Write every byte or report why not; errno is left set for the caller.
bool WriteFully(int fd, std::string_view data);

// mkstemp over a pattern string. `path` always receives the expanded template,
// including on failure — callers name it in their error message.
int CreateTempFile(const std::string& pattern, std::string& path);

// RAII mkstemp: owns the file descriptor and unlinks the path on destruction
// unless released. Replaces the hand-rolled Fd-plus-unlink dances, where an
// early return could either leak the file or unlink one the caller kept.
class ScopedTempFile {
 public:
  explicit ScopedTempFile(std::string pattern);
  ~ScopedTempFile();
  ScopedTempFile(ScopedTempFile&& other) noexcept;
  ScopedTempFile& operator=(ScopedTempFile&& other) noexcept;
  ScopedTempFile(const ScopedTempFile&) = delete;
  ScopedTempFile& operator=(const ScopedTempFile&) = delete;

  explicit operator bool() const { return static_cast<bool>(fd_); }
  int Get() const { return fd_.Get(); }
  const std::string& Path() const { return path_; }
  // Close early; the path is still unlinked on destruction unless released.
  void Close();
  // Keep the file and hand over its path. Discard the result only when the
  // path is already held elsewhere.
  std::string Release();
  // Hand over the descriptor; the file persists and the path is forgotten.
  int ReleaseFd();

 private:
  // path_ precedes fd_: the constructor fills path_ while creating fd_,
  // and members initialize in declaration order.
  std::string path_;
  Fd fd_;
  bool keep_ = false;
};

// Read at most `cap` bytes from an open stream — one byte further, so a longer
// source is detectable — cut back to a UTF-8 boundary. Returns whether the
// source had more to give.
bool ReadBounded(std::istream& input, size_t cap, std::string& out);

// Private state and web assets must never follow a final symlink or read a
// device/FIFO. A prefix read is useful for cheap catalogue headers.
bool ReadRegularFile(const std::string& path, size_t cap,
                     std::string& out, std::string& error,
                     bool prefix = false);

std::string UagentConfigPath();

// The path is stable even before the file exists: scratch state may also create
// .uagent, but only this specific file opts a workspace into local settings.
std::string ProjectConfigFilePath();

// Atomic shared writer for config, trust state, tools, and preferences. A temp
// file in the target directory makes replacement crash-safe.
bool AtomicWriteFile(const std::string& path, const std::string& content,
                            mode_t create_mode, bool preserve_mode,
                            std::string& error, bool overwrite = true);

// create_directories() applies the ambient umask, so hardening only the leaf
// leaves every directory it had to create along the way world-traversable.
// Every private path is built through here so the whole chain is owner-only.
void CreatePrivateDirectories(const std::filesystem::path& dir);

std::string MakePrivateDir(const std::string& base, const char* sub);

// ~/.uagent/<sub>, created on demand. History, sessions, logs, the trust store,
// and preferences stay global: a workspace must not be able to relocate — or
// grant itself — any of them.
std::string UagentDir(const char* sub);

// Artifact pruning walks whole directory trees; it lives in src/core/fs.cc so
// that every includer of this header stops compiling the walk.

// Delete `<session>.events.jsonl` journals whose session file is gone.
void PruneSessionJournalOrphans(const std::string& dir);

// Age- and count-bound every directory the agent writes to.
void MaintainArtifacts();

// Restrict to [A-Za-z0-9_-] and cap the length. The cap differs by consumer:
// file components and protocol-visible tool names have different limits.
std::string SanitizeComponent(std::string value, size_t cap);

std::string SafeFileComponent(std::string value);

std::string CanonicalCwd();

// Unique per process and per call. The counter it needs is process-wide
// mutable state, so the definition lives in src/core/fs.cc.
std::string MakeSessionId();

std::string WorkspaceId(const std::string& root);

bool LockFileExclusive(int fd);

bool AppendPrivateLine(const std::string& path, const std::string& line,
                              std::string& error);

// Atomically drain a small private append-only file. Truncating the locked
// inode instead of unlinking it keeps writers that opened before the lock from
// appending to an unreachable file.
bool TakePrivateText(const std::string& path, std::string& content,
                            std::string& error);

std::filesystem::path CanonicalAccessPath(const std::string& path);

std::string DisplayPath(const std::string& path);

bool PathWithin(const std::filesystem::path& path,
                       const std::filesystem::path& root);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_FS_H_
