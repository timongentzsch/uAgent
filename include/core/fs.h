// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FS_H_
#define UAGENT_INCLUDE_CORE_FS_H_
// Agent directories and crash-safe file writing. Global state stays under
// ~/.uagent; explicitly folder-scoped files use the folder's .uagent.

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
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/fd.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/strings.h"

namespace uagent {

// The throwing overload aborts under -fno-exceptions on a lookup error
// (EACCES on a parent, ELOOP). A path that cannot be looked up is not there.
bool PathExists(const std::filesystem::path& path);

std::string UserHome();

std::string Tilde(const std::string& path);
// Resolve a user-entered directory, including ~ and an empty home path.
std::string ExpandHome(std::string path);

// ~/.uagent. Falls back to a per-uid temp directory when the account has no
// home, never to a folder-controlled location.
std::string GlobalBase();
// Where session runtimes keep their sockets and leases. AF_UNIX paths are
// limited to 104 bytes on macOS, independently of HOME.
std::string RuntimeDir();

// The directory a folder opts into.
std::filesystem::path ProjectBase(const std::filesystem::path& cwd);

// Creates a private directory, following no symlink; checks owner and mode.
bool EnsurePrivateDirectory(const std::string& path);

// The agent's directory layout: the names more than one file spells.
inline constexpr const char* kMemoryDir = "memory";
inline constexpr const char* kHistoryDir = "history";
inline constexpr const char* kWorktreesDir = "worktrees";
inline constexpr const char* kSessionsDir = "sessions";
inline constexpr const char* kActivityDir = "activity";
inline constexpr const char* kTerminalsDir = "terminals";
// Below the records, not beside them: a record names a log to remove and a
// group to kill, so only the logs directory can be made writable.
inline constexpr const char* kTerminalLogsDir = "terminals/logs";
inline constexpr const char* kArtifactsDir = "artifacts";
inline constexpr const char* kMcpDir = "mcp";
inline constexpr const char* kModelsDir = "models";
inline constexpr const char* kConfigDir = "config";
inline constexpr const char* kMailDir = "mail";
// Named workspaces, one folder each.
inline constexpr const char* kSpacesDir = "spaces";
// The workspace a conversation starts in until it is filed.
inline constexpr const char* kUnfiledSpace = "Scratch";
inline constexpr const char* kWebDir = "web";
// Imports build here; the pruner empties what a killed import left.
inline constexpr const char* kStagingDir = "staging";
// Session drafts sit in the sessions folder of a host, beside its records.
inline constexpr const char* kDraftsDir = "drafts";
inline constexpr const char* kPinsFile = "pins.json";
// What a workspace keeps for itself, under its root.
inline constexpr const char* kWorkspaceDir = ".uagent";
inline constexpr const char* kScratchDir = "scratch";
inline std::filesystem::path ScratchDir(
    const std::filesystem::path& workspace) {
  return workspace / kWorkspaceDir / kScratchDir;
}
// Leases on a store's directory; the scheduler reads the host's by name.
inline constexpr const char* kMasterLockFile = "master.lock";
inline constexpr const char* kWriteLockFile = "write.lock";
// Sidecars of a session record (<id>.json), which the pruner must recognise.
inline constexpr const char* kSessionLockSuffix = ".lock";
inline constexpr const char* kSessionJournalSuffix = ".events.jsonl";
inline constexpr const char* kSessionAssetsSuffix = ".assets";
inline constexpr const char* kSessionEditsSuffix = ".edits";
// Plan mode's plan, the one file a conversation in it may write.
inline constexpr const char* kSessionPlanSuffix = ".plan.md";
// Remembered approval rules under kConfigDir; they grant future authority.
inline constexpr const char* kPermissionStoreFile = "permissions.json";

// mkstemp; `path` receives the expanded template even on failure.
int CreateTempFile(const std::string& pattern, std::string& path);

// RAII mkstemp: unlinks the path on destruction unless released.
class ScopedTempFile {
 public:
  explicit ScopedTempFile(const std::string& pattern);
  ~ScopedTempFile();
  ScopedTempFile(const ScopedTempFile&) = delete;
  ScopedTempFile& operator=(const ScopedTempFile&) = delete;

  explicit operator bool() const { return static_cast<bool>(fd_); }
  int Get() const { return fd_.Get(); }
  const std::string& Path() const { return path_; }
  // Close early; the path is still unlinked on destruction unless released.
  void Close();
  // Keeps the file and hands over its path.
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

// Reads at most `cap` bytes, cut back to a UTF-8 boundary; returns whether
// the source had more to give.
bool ReadBounded(std::istream& input, size_t cap, std::string& out);

// `path` resolved, when it is an existing directory.
std::optional<std::filesystem::path> CanonicalDirectory(
    const std::filesystem::path& path);

// A file's first `cap` bytes, cut to UTF-8; for files the caller trusts.
std::optional<std::string> ReadFile(const std::string& path, size_t cap);

// A regular file this user owns, opened without following a final symlink;
// empty when it is anything else or larger than `cap` bytes.
Fd OpenOwnedRegular(const std::string& path, size_t cap,
                    size_t* size = nullptr);
bool ReadRegularFile(const std::string& path, size_t cap, std::string& out,
                     std::string& error, bool prefix = false);

// Atomic replace through a temp file in the target directory.
bool AtomicWriteFile(const std::string& path, const std::string& content,
                     mode_t create_mode, bool preserve_mode, std::string& error,
                     bool overwrite = true);
// The same for agent-private state: owner-only, whatever mode it had.
bool WritePrivateFile(const std::string& path, const std::string& content,
                      std::string& error);

// Owner-only for the whole chain: create_directories() follows the umask.
void CreatePrivateDirectories(const std::filesystem::path& dir);

std::string MakePrivateDir(const std::string& base, const char* sub);

// ~/.uagent/<sub>, created on demand. Global on purpose: a folder must not be
// able to relocate, or grant itself, history, sessions, trust or settings.
std::string UagentDir(const char* sub);

// Delete `<session>.events.jsonl` journals whose session file is gone.
void PruneSessionJournalOrphans(const std::string& dir);

// Age- and count-bound every directory the agent writes to.
void MaintainArtifacts();
// MaintainArtifacts at most once a day, however many sessions start.
void MaintainArtifactsDaily();

// Restrict to [A-Za-z0-9_-] and cap the length.
std::string SanitizeComponent(std::string value, size_t cap);

std::string SafeFileComponent(std::string value);

std::string CanonicalCwd();

// Unique per process and per call.
std::string UniqueSeed();
// The same, hashed and named as a session.
std::string MakeSessionId();
// OS randomness for credentials and opaque identities. Empty on failure.
std::string RandomToken(size_t bytes = 24);

std::string WorkspaceId(const std::string& root, const std::string& host = {});

bool LockFileExclusive(int fd);

// The last `bytes` of a file, or all of it when `bytes` is negative; `start`
// receives the offset read from. Empty when the file cannot be opened.
std::string ReadFileTail(const std::string& path, int64_t bytes,
                         int64_t* start = nullptr);

bool AppendPrivateLine(const std::string& path, const std::string& line,
                       std::string& error);

// Atomically drains a small private append-only file. The locked inode is
// truncated, not unlinked, so earlier openers never append to a lost file.
bool TakePrivateText(const std::string& path, std::string& content,
                     std::string& error);

std::filesystem::path CanonicalAccessPath(const std::string& path);

std::string DisplayPath(const std::string& path);

bool PathWithin(const std::filesystem::path& path,
                const std::filesystem::path& root);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_FS_H_
