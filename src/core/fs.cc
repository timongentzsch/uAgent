// Copyright 2026 Timon Gentzsch

#include "include/core/fs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

template <typename Visit>
void ForEachTreeEntry(const std::string& dir, Visit&& visit) {
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
  PruneArtifactTree(UagentDir(kBgDir), BgDays(), BgFiles());
  PruneArtifactTree(UagentDir(kArtifactsDir), BgDays(), BgFiles());
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

}  // namespace uagent
