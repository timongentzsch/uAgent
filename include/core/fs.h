// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FS_H_
#define UAGENT_INCLUDE_CORE_FS_H_
// Agent directories and crash-safe file writing. Global state stays under
// ~/.uagent; explicitly project-scoped files use the workspace's .uagent.

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <istream>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/env.h"
#include "include/core/platform.h"
#include "include/core/strings.h"

namespace uagent {

inline std::string UserHome() {
  const char* home = getenv("HOME");
  if (home && *home) return home;
  int64_t buffer_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (buffer_size < 0) buffer_size = 16 * 1024;
  std::vector<char> buffer(static_cast<size_t>(buffer_size));
  struct passwd entry{};
  struct passwd* result = nullptr;
  if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) !=
      0) {
    return "";
  }
  return result && entry.pw_dir ? entry.pw_dir : "";
}

inline std::string Tilde(const std::string& path) {
  std::string home = UserHome();
  if (!home.empty() && path.starts_with(home)) {
    return "~" + path.substr(home.size());
  }
  return path;
}

// ~/.uagent. Falls back to a per-uid temp directory when the account has no
// home, never to a project-controlled location.
inline std::string GlobalBase() {
  std::string home = UserHome();
  return home.empty() ? "/tmp/uagent-" + std::to_string(getuid()) + "/.uagent"
                      : home + "/.uagent";
}

// The directory a workspace opts into. Named once: several modules need it,
// and a reader and a writer disagreeing about it would silently lose data.
inline std::filesystem::path ProjectBase(const std::filesystem::path& cwd) {
  return cwd / ".uagent";
}

// The agent's directory layout. Named where a second file also spells the
// name: a writer, a reader and the pruner disagreeing would strand data
// somewhere nothing looks. Single-use names stay literal at their one site.
inline constexpr const char* kMemoryDir = "memory";
inline constexpr const char* kHistoryDir = "history";
inline constexpr const char* kSessionsDir = "sessions";
inline constexpr const char* kBgDir = "bg";
inline constexpr const char* kTerminalsDir = "terminals";
inline constexpr const char* kArtifactsDir = "artifacts";
inline constexpr const char* kMcpDir = "mcp";
inline constexpr const char* kConfigDir = "config";

// Write every byte or report why not; errno is left set for the caller.
inline bool WriteFully(int fd, const std::string& data) {
  return WriteAll(fd, data.data(), data.size());
}

// mkstemp over a pattern string. `path` always receives the expanded template,
// including on failure — callers name it in their error message.
inline int CreateTempFile(const std::string& pattern, std::string& path) {
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  int fd = mkstemp(buffer.data());
  path = buffer.data();
  return fd;
}

// Read at most `cap` bytes from an open stream — one byte further, so a longer
// source is detectable — cut back to a UTF-8 boundary. Returns whether the
// source had more to give.
inline bool ReadBounded(std::istream& input, size_t cap, std::string& out) {
  out.assign(cap + 1, '\0');
  input.read(out.data(), static_cast<std::streamsize>(out.size()));
  size_t read = static_cast<size_t>(input.gcount());
  out.resize(std::min(read, cap));
  out = Utf8Prefix(std::move(out), cap);
  return read > cap;
}

inline std::string UagentConfigPath() {
  std::string home = UserHome();
  return home.empty() ? "" : home + "/.uagent/.config";
}

// The path is stable even before the file exists: scratch state may also create
// .uagent, but only this specific file opts a workspace into local settings.
inline std::string ProjectConfigFilePath() {
  std::error_code ec;
  std::filesystem::path cwd = std::filesystem::current_path(ec);
  return ec ? "" : (ProjectBase(cwd) / ".config").string();
}

// Atomic shared writer for config, trust state, tools, and preferences. A temp
// file in the target directory makes replacement crash-safe.
inline bool AtomicWriteFile(const std::string& path, const std::string& content,
                            mode_t create_mode, bool preserve_mode,
                            std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path target(path);
  if (target.has_parent_path()) {
    fs::create_directories(target.parent_path(), ec);
  }
  if (ec) {
    error = "cannot create parent directory for " + path + ": " + ec.message();
    return false;
  }
  if (fs::is_symlink(target, ec)) {
    target = fs::canonical(target, ec);
    if (ec) {
      error = "cannot resolve symlink " + path + ": " + ec.message();
      return false;
    }
  }
  fs::path parent =
      target.has_parent_path() ? target.parent_path() : fs::path(".");
  std::string temp;
  int fd = CreateTempFile(
      (parent / ("." + target.filename().string() + ".uagent.XXXXXX")).string(),
      temp);
  if (fd < 0) {
    error = "cannot create temporary file for " + path + ": " + strerror(errno);
    return false;
  }
  // `message` is built by the caller before unlink() can clobber errno.
  auto fail = [&](std::string message) {
    error = std::move(message);
    unlink(temp.c_str());
    return false;
  };
  struct stat st{};
  mode_t mode = preserve_mode && stat(target.c_str(), &st) == 0
                    ? st.st_mode & 07777
                    : create_mode;
  if (fchmod(fd, mode) != 0) {
    std::string message = strerror(errno);
    close(fd);
    return fail(message);
  }
  if (!WriteFully(fd, content)) {
    std::string message = "write to " + path + " failed: " + strerror(errno);
    close(fd);
    return fail(message);
  }
  int failure = fsync(fd) != 0 ? errno : 0;
  if (close(fd) != 0 && !failure) failure = errno;
  if (failure) {
    return fail("write to " + path + " failed: " + strerror(failure));
  }
  if (rename(temp.c_str(), target.c_str()) != 0) {
    return fail("cannot replace " + path + ": " + strerror(errno));
  }
  return true;
}

inline std::string MakePrivateDir(const std::string& base, const char* sub) {
  std::string dir = base + "/" + sub;
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  chmod(base.c_str(), 0700);
  std::filesystem::create_directories(dir, ec);
  chmod(dir.c_str(), 0700);
  return dir;
}

// ~/.uagent/<sub>, created on demand. History, sessions, logs, the trust store,
// and preferences stay global: a workspace must not be able to relocate — or
// grant itself — any of them.
inline std::string UagentDir(const char* sub) {
  return MakePrivateDir(GlobalBase(), sub);
}

template <typename Visit>
inline void ForEachTreeEntry(const std::string& dir, Visit&& visit) {
  namespace fs = std::filesystem;
  std::error_code ec;
  for (fs::recursive_directory_iterator
           it(dir, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    visit(*it);
  }
}

inline void PruneArtifactTree(const std::string& dir, int64_t max_age_days,
                              int64_t max_files) {
  namespace fs = std::filesystem;
  struct Entry {
    fs::path path;
    fs::file_time_type modified;
  };
  struct NewerFirst {
    bool operator()(const Entry& a, const Entry& b) const {
      return a.modified > b.modified;  // oldest entry at the top
    }
  };
  std::priority_queue<Entry, std::vector<Entry>, NewerFirst> kept;
  auto cutoff = fs::file_time_type::clock::now() -
                std::chrono::hours(24 * std::max(int64_t{1}, max_age_days));
  ForEachTreeEntry(dir, [&](const fs::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_symlink(ec)) return;
    if (entry.is_directory(ec)) {
      chmod(entry.path().c_str(), 0700);
    } else if (entry.is_regular_file(ec)) {
      chmod(entry.path().c_str(), 0600);
      std::error_code time_error;
      Entry artifact{entry.path(), entry.last_write_time(time_error)};
      if (time_error) return;
      if (artifact.modified < cutoff) {
        std::error_code remove_error;
        fs::remove(artifact.path, remove_error);
        return;
      }
      bool session_sidecar = artifact.path.string().ends_with(".events.jsonl");
      if (max_files > 0 && !session_sidecar) {
        kept.push(std::move(artifact));
        if (kept.size() > static_cast<size_t>(max_files)) {
          std::error_code remove_error;
          fs::remove(kept.top().path, remove_error);
          kept.pop();
        }
      }
    }
  });
}

inline void PruneSessionJournalOrphans(const std::string& dir) {
  namespace fs = std::filesystem;
  constexpr std::string_view kSuffix = ".events.jsonl";
  ForEachTreeEntry(dir, [&](const fs::directory_entry& entry) {
    std::error_code ec;
    if (!entry.is_regular_file(ec)) return;
    std::string path = entry.path().string();
    if (!path.ends_with(kSuffix)) return;
    std::string session = path.substr(0, path.size() - kSuffix.size());
    std::error_code exists_error;
    if (!fs::exists(session, exists_error) && !exists_error) {
      std::error_code remove_error;
      fs::remove(path, remove_error);
    }
  });
}

inline void MaintainArtifacts() {
  std::string history = UagentDir(kHistoryDir);
  PruneArtifactTree(history, HistoryDays(), HistoryFiles());
  PruneSessionJournalOrphans(history);
  PruneArtifactTree(GlobalBase() + "/" + kMemoryDir + "/.processed",
                    HistoryDays(), HistoryFiles());
  PruneArtifactTree(UagentDir(kSessionsDir), DebugDays(), DebugFiles());
  PruneArtifactTree(UagentDir(kBgDir), BgDays(), BgFiles());
  PruneArtifactTree(UagentDir(kArtifactsDir), BgDays(), BgFiles());
  PruneArtifactTree(UagentDir(kMcpDir), McpLogDays(), McpLogFiles());
}

// Restrict to [A-Za-z0-9_-] and cap the length. The cap differs by consumer:
// file components and protocol-visible tool names have different limits.
inline std::string SanitizeComponent(std::string value, size_t cap) {
  for (char& c : value) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      c = '_';
    }
  }
  if (value.size() > cap) value.resize(cap);
  return value;
}

inline std::string SafeFileComponent(std::string value) {
  if (value.empty()) return "unnamed";
  return SanitizeComponent(std::move(value), 80);
}

inline std::string CanonicalCwd() {
  std::error_code ec;
  auto path =
      std::filesystem::weakly_canonical(std::filesystem::current_path(), ec);
  return ec ? std::filesystem::current_path().string() : path.string();
}

inline std::string MakeSessionId() {
  static std::atomic<uint64_t> sequence{0};
  return "uagent-" +
         HashHex(
             CanonicalCwd() + ":" + std::to_string(getpid()) + ":" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ":" + std::to_string(++sequence));
}

inline std::string WorkspaceId(const std::string& root) {
  return HashHex(root);
}

inline bool LockFileExclusive(int fd) {
  int result;
  do {
    result = flock(fd, LOCK_EX);
  } while (result != 0 && errno == EINTR);
  return result == 0;
}

inline bool AppendPrivateLine(const std::string& path, const std::string& line,
                              std::string& error) {
  int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (fd < 0) {
    error = strerror(errno);
    return false;
  }
  fchmod(fd, 0600);
  if (!LockFileExclusive(fd)) {
    error = strerror(errno);
    close(fd);
    return false;
  }
  if (!WriteFully(fd, line + "\n")) {
    error = strerror(errno);
    flock(fd, LOCK_UN);
    close(fd);
    return false;
  }
  flock(fd, LOCK_UN);
  close(fd);
  return true;
}

// Atomically drain a small private append-only file. Truncating the locked
// inode instead of unlinking it keeps writers that opened before the lock from
// appending to an unreachable file.
inline bool TakePrivateText(const std::string& path, std::string& content,
                            std::string& error) {
  content.clear();
  int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return true;
    error = strerror(errno);
    return false;
  }
  if (!LockFileExclusive(fd)) {
    error = strerror(errno);
    close(fd);
    return false;
  }
  constexpr size_t kMaxBytes = 16 * 1024 * 1024;
  char buffer[4096];
  bool ok = true;
  for (;;) {
    ssize_t count = read(fd, buffer, sizeof buffer);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      error = strerror(errno);
      ok = false;
      break;
    }
    if (count == 0) break;
    size_t bytes = static_cast<size_t>(count);
    if (AdditionExceeds(content.size(), bytes, kMaxBytes)) {
      error = "private file exceeds 16 MiB";
      ok = false;
      break;
    }
    content.append(buffer, bytes);
  }
  if (ok && ftruncate(fd, 0) != 0) {
    error = strerror(errno);
    ok = false;
  }
  flock(fd, LOCK_UN);
  close(fd);
  return ok;
}

inline std::filesystem::path CanonicalAccessPath(const std::string& path) {
  std::error_code ec;
  std::filesystem::path p = path.empty() ? "." : path;
  auto canonical = std::filesystem::weakly_canonical(p, ec);
  return ec ? std::filesystem::absolute(p, ec) : canonical;
}

inline std::string DisplayPath(const std::string& path) {
  std::error_code error;
  std::filesystem::path relative = std::filesystem::relative(
      CanonicalAccessPath(path), CanonicalCwd(), error);
  if (error || relative.empty() ||
      (relative.begin() != relative.end() && *relative.begin() == "..")) {
    return path;
  }
  return relative.string();
}

inline bool PathWithin(const std::filesystem::path& path,
                       const std::filesystem::path& root) {
  auto p = path.lexically_normal();
  auto r = root.lexically_normal();
  auto pi = p.begin(), ri = r.begin();
  for (; ri != r.end(); ++ri, ++pi) {
    if (pi == p.end() || *pi != *ri) return false;
  }
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_FS_H_
