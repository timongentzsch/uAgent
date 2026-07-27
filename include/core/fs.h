// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FS_H_
#define UAGENT_INCLUDE_CORE_FS_H_
// Agent directories and crash-safe file writing. The base directory is the
// workspace's .uagent when it exists, else ~/.uagent; paths are built here
// so no caller has to know which one is in play.

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
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

// ~/.uagent. Falls back to a per-uid temp directory when the account has no
// home, never to a project-controlled location.
inline std::string GlobalBase() {
  std::string home = UserHome();
  return home.empty() ? "/tmp/uagent-" + std::to_string(getuid()) + "/.uagent"
                      : home + "/.uagent";
}

// The workspace's ./.uagent when that directory already exists, else the global
// one. Never created here: a project opts in by making the directory itself.
// Recomputed per call rather than cached, so tests can move HOME and cwd.
inline std::string UagentBase() {
  std::error_code ec;
  std::filesystem::path local = std::filesystem::current_path(ec) / ".uagent";
  if (!ec && std::filesystem::is_directory(local, ec)) return local.string();
  return GlobalBase();
}

inline std::string UagentConfigPath() {
  std::string home = UserHome();
  return home.empty() ? "" : home + "/.uagent/.config";
}

// Project settings live beside the workspace, and only when it opted in.
inline std::string ProjectConfigFilePath() {
  std::string base = UagentBase();
  return base == GlobalBase() ? "" : base + "/.config";
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
  std::string pattern =
      (parent / ("." + target.filename().string() + ".uagent.XXXXXX")).string();
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back('\0');
  int fd = mkstemp(temp.data());
  if (fd < 0) {
    error = "cannot create temporary file for " + path + ": " + strerror(errno);
    return false;
  }
  // `message` is built by the caller before unlink() can clobber errno.
  auto fail = [&](std::string message) {
    error = std::move(message);
    unlink(temp.data());
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
  for (size_t offset = 0; offset < content.size();) {
    ssize_t written =
        write(fd, content.data() + offset, content.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      std::string message = "write to " + path + " failed: " + strerror(errno);
      close(fd);
      return fail(message);
    }
    offset += static_cast<size_t>(written);
  }
  int failure = fsync(fd) != 0 ? errno : 0;
  if (close(fd) != 0 && !failure) failure = errno;
  if (failure) {
    return fail("write to " + path + " failed: " + strerror(failure));
  }
  if (rename(temp.data(), target.c_str()) != 0) {
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

// ./.uagent/<sub> when the workspace opted in, else the global directory. Only
// memories and the uv cache follow the workspace.
inline std::string UagentScopedDir(const char* sub) {
  return MakePrivateDir(UagentBase(), sub);
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
  std::error_code ec;
  auto cutoff = fs::file_time_type::clock::now() -
                std::chrono::hours(24 * std::max(int64_t{1}, max_age_days));
  for (fs::recursive_directory_iterator
           it(dir, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_symlink(ec)) continue;
    if (it->is_directory(ec)) {
      chmod(it->path().c_str(), 0700);
    } else if (it->is_regular_file(ec)) {
      chmod(it->path().c_str(), 0600);
      std::error_code time_error;
      Entry entry{it->path(), it->last_write_time(time_error)};
      if (time_error) continue;
      if (entry.modified < cutoff) {
        std::error_code remove_error;
        fs::remove(entry.path, remove_error);
        continue;
      }
      if (max_files > 0) {
        kept.push(std::move(entry));
        if (kept.size() > static_cast<size_t>(max_files)) {
          std::error_code remove_error;
          fs::remove(kept.top().path, remove_error);
          kept.pop();
        }
      }
    }
  }
}

inline void MaintainArtifacts() {
  PruneArtifactTree(UagentDir("history"), HistoryDays(), HistoryFiles());
  PruneArtifactTree(UagentDir("sessions"), DebugDays(), DebugFiles());
  PruneArtifactTree(UagentDir("bg"), BgDays(), BgFiles());
  PruneArtifactTree(UagentDir("mcp"), McpLogDays(), McpLogFiles());
}

inline std::string SafeFileComponent(std::string value) {
  for (char& c : value) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      c = '_';
    }
  }
  if (value.empty()) value = "unnamed";
  if (value.size() > 80) value.resize(80);
  return value;
}

inline std::string CanonicalCwd() {
  std::error_code ec;
  auto path =
      std::filesystem::weakly_canonical(std::filesystem::current_path(), ec);
  return ec ? std::filesystem::current_path().string() : path.string();
}

inline std::string MakeSessionId() {
  static std::atomic<uint64_t> sequence{0};
  std::string seed =
      CanonicalCwd() + ":" + std::to_string(getpid()) + ":" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      ":" + std::to_string(++sequence);
  return "uagent-" +
         Hex64(Fnv1aUpdate(1469598103934665603ULL, seed.data(), seed.size()));
}

inline std::string WorkspaceId(const std::string& root) {
  return Hex64(Fnv1aUpdate(1469598103934665603ULL, root.data(), root.size()));
}

inline bool AppendPrivateLine(const std::string& path, const std::string& line,
                              std::string& error) {
  int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (fd < 0) {
    error = strerror(errno);
    return false;
  }
  fchmod(fd, 0600);
  if (flock(fd, LOCK_EX) != 0) {
    error = strerror(errno);
    close(fd);
    return false;
  }
  std::string record = line + "\n";
  size_t offset = 0;
  while (offset < record.size()) {
    ssize_t written = write(fd, record.data() + offset, record.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      error = strerror(errno);
      flock(fd, LOCK_UN);
      close(fd);
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  flock(fd, LOCK_UN);
  close(fd);
  return true;
}

inline std::filesystem::path CanonicalAccessPath(const std::string& path) {
  std::error_code ec;
  std::filesystem::path p = path.empty() ? "." : path;
  auto canonical = std::filesystem::weakly_canonical(p, ec);
  return ec ? std::filesystem::absolute(p, ec) : canonical;
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
