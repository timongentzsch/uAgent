// Copyright 2026 Timon Gentzsch

#include "include/core/fs.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <istream>
#include <map>
#include <queue>
#include <string>
#include <string_view>
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
bool PathExists(const std::filesystem::path& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored);
}

std::string UserHome() {
  const char* home = getenv("HOME");
  if (home && *home) return home;
  int64_t buffer_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (buffer_size < 0) buffer_size = int64_t{16} * 1024;
  std::vector<char> buffer(static_cast<size_t>(buffer_size));
  struct passwd entry{};
  struct passwd* result = nullptr;
  if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) !=
      0) {
    return "";
  }
  return result && entry.pw_dir ? entry.pw_dir : "";
}

std::string Tilde(const std::string& path) {
  std::string home = UserHome();
  if (!home.empty() && path.starts_with(home)) {
    return "~" + path.substr(home.size());
  }
  return path;
}

// ~/.uagent. Falls back to a per-uid temp directory when the account has no
// home, never to a project-controlled location.
std::string GlobalBase() {
  std::string home = UserHome();
  return home.empty() ? "/tmp/uagent-" + std::to_string(getuid()) + "/.uagent"
                      : home + "/.uagent";
}

// The directory a workspace opts into. Named once: several modules need it,
// and a reader and a writer disagreeing about it would silently lose data.
std::filesystem::path ProjectBase(const std::filesystem::path& cwd) {
  return cwd / ".uagent";
}

// Write every byte or report why not; errno is left set for the caller.
bool WriteFully(int fd, std::string_view data) {
  return WriteAll(fd, data.data(), data.size());
}

// mkstemp over a pattern string. `path` always receives the expanded template,
// including on failure - callers name it in their error message.
int CreateTempFile(const std::string& pattern, std::string& path) {
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  int fd = mkstemp(buffer.data());
  path = buffer.data();
  return fd;
}

ScopedTempFile::ScopedTempFile(const std::string& pattern)
    : path_(), fd_(CreateTempFile(pattern, path_)) {}

ScopedTempFile::~ScopedTempFile() {
  fd_.Reset();
  if (!keep_ && !path_.empty()) unlink(path_.c_str());
}

ScopedTempFile::ScopedTempFile(ScopedTempFile&& other) noexcept
    : path_(std::move(other.path_)),
      fd_(std::move(other.fd_)),
      keep_(std::exchange(other.keep_, true)) {}

ScopedTempFile& ScopedTempFile::operator=(ScopedTempFile&& other) noexcept {
  if (this != &other) {
    fd_.Reset();
    if (!keep_ && !path_.empty()) unlink(path_.c_str());
    fd_ = std::move(other.fd_);
    path_ = std::move(other.path_);
    keep_ = std::exchange(other.keep_, true);
  }
  return *this;
}

void ScopedTempFile::Close() { fd_.Reset(); }

std::string ScopedTempFile::Release() {
  fd_.Reset();
  keep_ = true;
  return path_;
}

int ScopedTempFile::ReleaseFd() {
  keep_ = true;
  path_.clear();
  return fd_.Release();
}

// Read at most `cap` bytes from an open stream - one byte further, so a
// longer source is detectable - cut back to a UTF-8 boundary. Returns
// whether the source had more to give.
bool ReadBounded(std::istream& input, size_t cap, std::string& out) {
  out.assign(cap + 1, '\0');
  input.read(out.data(), static_cast<std::streamsize>(out.size()));
  size_t read = static_cast<size_t>(input.gcount());
  out.resize(std::min(read, cap));
  out = Utf8Prefix(std::move(out), cap);
  return read > cap;
}

bool ReadRegularFile(const std::string& path, size_t cap, std::string& out,
                     std::string& error, bool prefix) {
  out.clear();
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  struct stat info{};
  if (!fd || fstat(fd.Get(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size < 0) {
    error = "cannot read regular file";
    return false;
  }
  if (!prefix && static_cast<uintmax_t>(info.st_size) > cap) {
    error = "file exceeds read limit";
    return false;
  }
  char buffer[8192];
  while (out.size() < cap) {
    ssize_t count =
        read(fd.Get(), buffer, std::min(sizeof buffer, cap - out.size()));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      error = strerror(errno);
      return false;
    }
    if (count == 0) return true;
    out.append(buffer, static_cast<size_t>(count));
  }
  if (prefix) return true;
  ssize_t extra;
  do {
    extra = read(fd.Get(), buffer, 1);
  } while (extra < 0 && errno == EINTR);
  if (extra == 0) return true;
  error = "file changed or exceeds read limit";
  return false;
}

std::string UagentConfigPath() {
  std::string home = UserHome();
  return home.empty() ? "" : home + "/.uagent/.config";
}

std::string ProjectConfigFilePath() {
  std::error_code ec;
  std::filesystem::path cwd = std::filesystem::current_path(ec);
  return ec ? "" : (ProjectBase(cwd) / ".config").string();
}

bool EnsurePrivateDirectory(const std::string& path) {
  std::filesystem::path current = GlobalBase();
  auto relative = std::filesystem::path(path).lexically_relative(current);
  if (relative.empty() || relative.is_absolute()) return false;
  for (const auto& part : relative) {
    if (part == "..") return false;
  }
  std::error_code ec;
  if (std::filesystem::is_symlink(
          std::filesystem::symlink_status(current, ec))) {
    return false;
  }
  for (const auto& part : relative) {
    current /= part;
    if (std::filesystem::is_symlink(
            std::filesystem::symlink_status(current, ec))) {
      return false;
    }
  }
  CreatePrivateDirectories(path);
  struct stat info{};
  return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
         info.st_uid == geteuid() && (info.st_mode & 0077) == 0;
}
namespace {

template <typename Visit>
void ForEachTreeEntry(const std::string& dir, Visit&& visit) {
  // NOLINTNEXTLINE(misc-unused-alias-decls) used below; the check misses it
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

void PruneArtifactTree(const std::string& dir, int64_t max_age_days,
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
      chmod(entry.path().c_str(), kPrivateDirMode);
    } else if (entry.is_regular_file(ec)) {
      chmod(entry.path().c_str(), kPrivateFileMode);
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

void PruneCollaboratorTree(const std::string& dir, int64_t max_age_days,
                           int64_t max_records) {
  namespace fs = std::filesystem;
  struct Record {
    fs::file_time_type modified = fs::file_time_type::min();
    std::vector<fs::path> files;
  };
  std::map<std::string, Record, std::less<>> records;
  ForEachTreeEntry(dir, [&](const fs::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_symlink(ec)) return;
    if (entry.is_directory(ec)) {
      chmod(entry.path().c_str(), kPrivateDirMode);
      return;
    }
    if (!entry.is_regular_file(ec)) return;
    chmod(entry.path().c_str(), kPrivateFileMode);
    std::string name = entry.path().filename().string();
    constexpr std::string_view kSessionSuffix = ".session.json";
    constexpr std::string_view kRecordSuffix = ".json";
    size_t suffix =
        name.ends_with(kSessionSuffix)
            ? kSessionSuffix.size()
            : (name.ends_with(kRecordSuffix) ? kRecordSuffix.size() : 0);
    if (suffix == 0) return;
    std::error_code time_error;
    fs::file_time_type modified = entry.last_write_time(time_error);
    if (time_error) return;
    std::string base = name.substr(0, name.size() - suffix);
    // Undelivered mail belongs to its recipient's group, so it ages out with
    // the record and, while it is fresh, keeps that record from looking stale.
    if (size_t mail = base.find(".mail-"); mail != std::string::npos) {
      base.resize(mail);
    }
    Record& record = records[base];
    record.modified = std::max(record.modified, modified);
    record.files.push_back(entry.path());
  });

  auto remove_record = [](const Record& record) {
    for (const fs::path& path : record.files) {
      std::error_code remove_error;
      fs::remove(path, remove_error);
    }
  };
  auto cutoff = fs::file_time_type::clock::now() -
                std::chrono::hours(24 * std::max(int64_t{1}, max_age_days));
  std::vector<std::string_view> kept;
  kept.reserve(records.size());
  for (const auto& [id, record] : records) {
    if (record.modified < cutoff) {
      remove_record(record);
    } else {
      kept.push_back(id);
    }
  }
  if (max_records <= 0 || kept.size() <= static_cast<size_t>(max_records)) {
    return;
  }
  // Newest first, ties broken by id. std::sort is not stable, so without a
  // total order two records stamped in the same filesystem tick could prune in
  // either direction from one run to the next -- which is why this keeps ids
  // rather than record addresses.
  std::sort(kept.begin(), kept.end(),
            [&records](std::string_view a, std::string_view b) {
              const Record& left = records.find(a)->second;
              const Record& right = records.find(b)->second;
              if (left.modified != right.modified) {
                return left.modified > right.modified;
              }
              return a < b;
            });
  for (size_t i = static_cast<size_t>(max_records); i < kept.size(); ++i) {
    remove_record(records.find(kept[i])->second);
  }
}

}  // namespace

void PruneSessionJournalOrphans(const std::string& dir) {
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

void MaintainArtifacts() {
  std::string history = UagentDir(kHistoryDir);
  PruneArtifactTree(history, HistoryDays(), HistoryFiles());
  PruneSessionJournalOrphans(history);
  PruneArtifactTree(GlobalBase() + "/" + kMemoryDir + "/.processed",
                    HistoryDays(), HistoryFiles());
  PruneArtifactTree(UagentDir(kSessionsDir), DebugDays(), DebugFiles());
  PruneArtifactTree(UagentDir(kSessionsDir) + "/inbox", DebugDays(),
                    DebugFiles());
  PruneArtifactTree(UagentDir(kBgDir), BgDays(), BgFiles());
  PruneArtifactTree(UagentDir(kArtifactsDir), BgDays(), BgFiles());
  PruneCollaboratorTree(UagentDir("collaborators"), DebugDays(), DebugFiles());
  PruneArtifactTree(UagentDir(kMcpDir), McpLogDays(), McpLogFiles());
}

std::string MakeSessionId() {
  static std::atomic<uint64_t> sequence{0};
  return "uagent-" +
         HashHex(
             CanonicalCwd() + ":" + std::to_string(getpid()) + ":" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ":" + std::to_string(++sequence));
}

bool AtomicWriteFile(const std::string& path, const std::string& content,
                     mode_t create_mode, bool preserve_mode, std::string& error,
                     bool overwrite) {
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
  if (overwrite && fs::is_symlink(target, ec)) {
    target = fs::canonical(target, ec);
    if (ec) {
      error = "cannot resolve symlink " + path + ": " + ec.message();
      return false;
    }
  }
  fs::path parent =
      target.has_parent_path() ? target.parent_path() : fs::path(".");
  std::string temp;
  Fd fd(CreateTempFile(
      (parent / ("." + target.filename().string() + ".uagent.XXXXXX")).string(),
      temp));
  if (!fd) {
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
  if (fchmod(fd.Get(), mode) != 0) return fail(strerror(errno));
  if (!WriteFully(fd.Get(), content)) {
    return fail("write to " + path + " failed: " + strerror(errno));
  }
  // A deferred write can still fail in close(2), so the commit is fsync plus
  // a close that returned zero.
  int failure = fsync(fd.Get()) != 0 ? errno : 0;
  if (fd.Close() != 0 && !failure) failure = errno;
  if (failure) {
    return fail("write to " + path + " failed: " + strerror(failure));
  }
  if ((overwrite ? rename(temp.c_str(), target.c_str())
                 : link(temp.c_str(), target.c_str())) != 0) {
    return fail("cannot write " + path + ": " + strerror(errno) +
                (errno == EEXIST ? "; use edit_file or overwrite=true" : ""));
  }
  if (!overwrite) unlink(temp.c_str());
  // The rename is already atomic; syncing the directory is what makes it
  // durable, so a crash cannot leave the entry pointing at nothing. A failure
  // here means the new contents may not survive power loss, not that the
  // replacement was lost, so it is reported without unlinking the new file.
  Fd directory(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory && fsync(directory.Get()) != 0) {
    error = "replaced " + path +
            " but could not sync its directory: " + strerror(errno);
    return false;
  }
  return true;
}
void CreatePrivateDirectories(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<fs::path> missing;
  for (fs::path walk = dir; !walk.empty() && !fs::exists(walk, ec);
       walk = walk.parent_path()) {
    missing.push_back(walk);
    if (!walk.has_relative_path()) break;
  }
  fs::create_directories(dir, ec);
  for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
    chmod(it->c_str(), kPrivateDirMode);
  }
}
std::string MakePrivateDir(const std::string& base, const char* sub) {
  CreatePrivateDirectories(base);
  std::string dir = base + "/" + sub;
  CreatePrivateDirectories(dir);
  return dir;
}
std::string UagentDir(const char* sub) {
  return MakePrivateDir(GlobalBase(), sub);
}
std::string SanitizeComponent(std::string value, size_t cap) {
  for (char& c : value) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      c = '_';
    }
  }
  if (value.size() > cap) value.resize(cap);
  return value;
}
std::string SafeFileComponent(std::string value) {
  if (value.empty()) return "unnamed";
  return SanitizeComponent(std::move(value), 80);
}
std::string CanonicalCwd() {
  std::error_code ec;
  auto path =
      std::filesystem::weakly_canonical(std::filesystem::current_path(), ec);
  return ec ? std::filesystem::current_path().string() : path.string();
}
std::string WorkspaceId(const std::string& root) { return HashHex(root); }
bool LockFileExclusive(int fd) {
  int result;
  do {
    result = flock(fd, LOCK_EX);
  } while (result != 0 && errno == EINTR);
  return result == 0;
}
bool AppendPrivateLine(const std::string& path, const std::string& line,
                       std::string& error) {
  Fd fd(open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, kPrivateFileMode));
  if (!fd) {
    error = strerror(errno);
    return false;
  }
  fchmod(fd.Get(), kPrivateFileMode);
  if (!LockFileExclusive(fd.Get())) {
    error = strerror(errno);
    return false;
  }
  const bool written = WriteFully(fd.Get(), line + "\n");
  if (!written) error = strerror(errno);
  flock(fd.Get(), LOCK_UN);
  return written;
}
bool TakePrivateText(const std::string& path, std::string& content,
                     std::string& error) {
  content.clear();
  Fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
  if (!fd) {
    if (errno == ENOENT) return true;
    error = strerror(errno);
    return false;
  }
  if (!LockFileExclusive(fd.Get())) {
    error = strerror(errno);
    return false;
  }
  constexpr size_t kMaxBytes = size_t{16} * 1024 * 1024;
  char buffer[4096];
  bool ok = true;
  for (;;) {
    ssize_t count = read(fd.Get(), buffer, sizeof buffer);
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
  if (ok && ftruncate(fd.Get(), 0) != 0) {
    error = strerror(errno);
    ok = false;
  }
  flock(fd.Get(), LOCK_UN);
  return ok;
}
std::filesystem::path CanonicalAccessPath(const std::string& path) {
  std::error_code ec;
  std::filesystem::path p = path.empty() ? "." : path;
  // Made absolute before resolving, because the two standard libraries
  // disagree about a relative path whose target does not exist: libc++
  // returns it absolute, libstdc++ returns it unchanged and reports no
  // error. Every caller compares the answer against an absolute root, so a
  // relative one reads as outside the workspace.
  std::filesystem::path rooted = std::filesystem::absolute(p, ec);
  if (ec) return p.lexically_normal();
  auto canonical = std::filesystem::weakly_canonical(rooted, ec);
  return ec ? rooted.lexically_normal() : canonical;
}
std::string DisplayPath(const std::string& path) {
  std::error_code error;
  std::filesystem::path relative = std::filesystem::relative(
      CanonicalAccessPath(path), CanonicalCwd(), error);
  if (error || relative.empty() ||
      (relative.begin() != relative.end() && *relative.begin() == "..")) {
    return path;
  }
  return relative.string();
}
bool PathWithin(const std::filesystem::path& path,
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
