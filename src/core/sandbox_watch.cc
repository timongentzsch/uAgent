// Copyright 2026 Timon Gentzsch

#include "include/core/sandbox_watch.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/core/events.h"
#include "include/core/limits.h"

namespace uagent {
namespace {

namespace fs = std::filesystem;

// A repository's hooks are a few sample scripts; anything beyond this is not
// a hooks folder worth copying into memory for every command.
constexpr size_t kWatchBytes = MiB(4);
constexpr size_t kWatchEntries = 1000;

// False when `path` is too big to record. Absent is an empty tree and fine.
bool Record(const fs::path& path, SandboxWatch::Tree* tree) {
  size_t bytes = 0;
  std::error_code ec;
  if (!fs::exists(fs::symlink_status(path, ec))) return true;
  std::vector<fs::path> pending{path};
  while (!pending.empty()) {
    const fs::path current = std::move(pending.back());
    pending.pop_back();
    if (tree->size() >= kWatchEntries) return false;
    SandboxWatch::Entry entry;
    entry.name = current == path ? "" : fs::relative(current, path).string();
    const fs::file_status status = fs::symlink_status(current, ec);
    struct stat info{};
    entry.mode = lstat(current.c_str(), &info) == 0 ? info.st_mode & 07777 : 0;
    if (fs::is_symlink(status)) {
      entry.kind = SandboxWatch::Entry::Kind::kLink;
      entry.content = fs::read_symlink(current, ec).string();
    } else if (fs::is_directory(status)) {
      entry.kind = SandboxWatch::Entry::Kind::kFolder;
      for (const fs::directory_entry& child :
           fs::directory_iterator(current, ec)) {
        pending.push_back(child.path());
      }
    } else if (fs::is_regular_file(status)) {
      std::ifstream file(current, std::ios::binary);
      std::ostringstream content;
      content << file.rdbuf();
      entry.content = content.str();
      bytes += entry.content.size();
      if (bytes > kWatchBytes) return false;
    } else {
      return false;
    }
    tree->push_back(std::move(entry));
  }
  std::sort(tree->begin(), tree->end(),
            [](const SandboxWatch::Entry& a, const SandboxWatch::Entry& b) {
              return a.name < b.name;
            });
  return true;
}

void Put(const fs::path& path, const SandboxWatch::Tree& tree) {
  std::error_code ec;
  fs::remove_all(path, ec);
  // Sorted by name, so a folder precedes what is inside it.
  for (const SandboxWatch::Entry& entry : tree) {
    const fs::path target = entry.name.empty() ? path : path / entry.name;
    switch (entry.kind) {
      case SandboxWatch::Entry::Kind::kFolder:
        fs::create_directories(target, ec);
        break;
      case SandboxWatch::Entry::Kind::kLink:
        fs::create_symlink(entry.content, target, ec);
        break;
      case SandboxWatch::Entry::Kind::kFile: {
        fs::create_directories(target.parent_path(), ec);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file << entry.content;
        break;
      }
    }
    if (entry.kind != SandboxWatch::Entry::Kind::kLink) {
      chmod(target.c_str(), static_cast<mode_t>(entry.mode));
    }
  }
}

}  // namespace

SandboxWatch::SandboxWatch(std::vector<std::string> paths) {
  for (std::string& path : paths) {
    Watched watched{std::move(path), {}, true};
    watched.guarded = Record(watched.path, &watched.tree);
    watched_.push_back(std::move(watched));
  }
}

std::vector<std::string> SandboxWatch::Restore(
    const std::string& workspace) const {
  std::vector<std::string> notices;
  for (const Watched& watched : watched_) {
    Tree now;
    if (!watched.guarded ||
        (Record(watched.path, &now) && now == watched.tree)) {
      continue;
    }
    Put(watched.path, watched.tree);
    const bool inside =
        !workspace.empty() && watched.path.starts_with(workspace + "/");
    notices.push_back(
        "restored " +
        (inside ? watched.path.substr(workspace.size() + 1) : watched.path) +
        ": sandboxed commands may not change it");
  }
  return notices;
}

std::string SandboxRestoreNote(const SandboxWatch* watch,
                               const std::string& workspace) {
  std::string note;
  if (!watch) return note;
  for (const std::string& restored : watch->Restore(workspace)) {
    Emit(NoticeEvent(PresentationStatus::kWarned, "sandbox: " + restored));
    note += "\n[sandbox: " + restored + "]";
  }
  return note;
}

}  // namespace uagent
