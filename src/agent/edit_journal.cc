// Copyright 2026 Timon Gentzsch

#include "include/agent/edit_journal.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/file_services.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

// Past either cap a change is still listed, as not undoable.
constexpr size_t kUndoFileBytes = MiB(2);
constexpr uint64_t kUndoSessionBytes = MiB(64);
constexpr size_t kSummaryFiles = 50;

// Whether `path` still holds what a turn left: bytes hashing to `hash`, or
// nothing at all for an empty hash.
bool Holds(const std::string& path, const std::string& hash) {
  std::error_code ec;
  if (hash.empty()) {
    return !std::filesystem::exists(std::filesystem::symlink_status(path, ec));
  }
  std::string content, error;
  return ReadRegularFile(path, kEditFileBytes, content, error) &&
         HashHex(content) == hash;
}

}  // namespace

void EditJournal::Open(std::string directory) {
  directory_ = std::move(directory);
  turns_.clear();
  stored_bytes_ = 0;
  std::string text, error;
  if (!ReadRegularFile(directory_ + "/index.json", kEditFileBytes, text,
                       error)) {
    return;
  }
  const json index = json::parse(text, nullptr, false);
  const json turns = JsonValue(index, "turns", json::object());
  for (const auto& [turn, entries] : turns.items()) {
    int64_t number = 0;
    if (!ParseInt64(turn.c_str(), number) || !entries.is_array()) continue;
    for (const json& entry : entries) {
      turns_[number].push_back({JsonValue(entry, "path", ""),
                                JsonValue(entry, "after_hash", ""),
                                JsonValue(entry, "existed", false),
                                JsonValue(entry, "undoable", false)});
    }
  }
  std::error_code ec;
  for (const auto& blob : std::filesystem::directory_iterator(directory_, ec)) {
    // A stored diff lives here too, outside the undo budget.
    if (blob.path().filename().string().starts_with("diff-")) continue;
    std::error_code size_error;
    stored_bytes_ += blob.file_size(size_error);
  }
}

std::string EditJournal::Blob(int64_t turn, const std::string& path) const {
  return directory_ + "/" + std::to_string(turn) + "-" + HashHex(path);
}

void EditJournal::Record(int64_t turn, const FileEffect& effect) {
  if (directory_.empty()) return;
  std::vector<Entry>& entries = turns_[turn];
  auto found = std::ranges::find(entries, effect.path, &Entry::path);
  if (found != entries.end()) {
    found->after_hash = effect.after_hash;
    return;
  }
  const std::string& before = effect.before ? *effect.before : std::string();
  Entry entry{effect.path, effect.after_hash, effect.existed,
              (!effect.existed || effect.before) &&
                  before.size() <= kUndoFileBytes &&
                  stored_bytes_ + before.size() <= kUndoSessionBytes};
  if (entry.undoable && entry.existed) {
    std::string error;
    CreatePrivateDirectories(directory_);
    entry.undoable = AtomicWriteFile(Blob(turn, effect.path), before,
                                     kPrivateFileMode, false, error);
    if (entry.undoable) stored_bytes_ += before.size();
  }
  entries.push_back(std::move(entry));
}

json EditJournal::Files(int64_t turn) {
  json files = json::array();
  auto found = turns_.find(turn);
  if (found == turns_.end()) return files;
  Save();
  for (const Entry& entry : found->second) {
    if (files.size() == kSummaryFiles) break;
    if (!entry.existed && entry.after_hash.empty()) continue;  // came and went
    std::string before, after, error;
    if (entry.undoable && entry.existed) {
      ReadRegularFile(Blob(turn, entry.path), kUndoFileBytes, before, error);
    }
    if (!entry.after_hash.empty()) {
      ReadRegularFile(entry.path, kEditFileBytes, after, error);
    }
    const auto old_lines = DiffLines(before);
    const auto new_lines = DiffLines(after);
    const CommonLineSpan span = TrimCommonLines(old_lines, new_lines);
    files.push_back({{"path", DisplayPath(entry.path)},
                     {"added", span.new_end - span.prefix},
                     {"removed", span.old_end - span.prefix},
                     {"undoable", entry.undoable}});
  }
  return files;
}

int64_t EditJournal::LastTurn() const {
  return turns_.empty() ? 0 : turns_.rbegin()->first;
}

json EditJournal::Revert(int64_t turn, const std::string& path) {
  json restored = json::array(), conflicts = json::array();
  auto found = turns_.find(turn);
  if (found == turns_.end()) {
    return {{"restored", restored}, {"conflicts", conflicts}};
  }
  std::erase_if(found->second, [&](const Entry& entry) {
    const std::string shown = DisplayPath(entry.path);
    if (!path.empty() && path != shown && path != entry.path) return false;
    std::string reason, error;
    if (!entry.undoable) {
      reason = "not kept (binary or too large)";
    } else if (!Holds(entry.path, entry.after_hash)) {
      reason = "changed since";
    } else if (!entry.existed) {
      std::error_code ec;
      if (!std::filesystem::remove(entry.path, ec) && ec) reason = ec.message();
    } else {
      std::string before;
      ToolResult write =
          ReadRegularFile(Blob(turn, entry.path), kUndoFileBytes, before, error)
              ? ToolAtomicWrite(entry.path, before, kSharedFileMode, true)
              : ToolFailure(ToolErrorCode::kNotFound, error);
      if (!write.Ok()) reason = write.output;
    }
    if (!reason.empty()) {
      conflicts.push_back({{"path", shown}, {"reason", reason}});
      return false;
    }
    std::error_code ec;
    std::filesystem::remove(Blob(turn, entry.path), ec);
    restored.push_back(shown);
    return true;
  });
  if (found->second.empty()) turns_.erase(found);
  Save();
  return {{"restored", restored}, {"conflicts", conflicts}};
}

void EditJournal::Save() const {
  json index = json::object();
  for (const auto& [turn, entries] : turns_) {
    json& rows = index[std::to_string(turn)] = json::array();
    for (const Entry& entry : entries) {
      rows.push_back({{"path", entry.path},
                      {"after_hash", entry.after_hash},
                      {"existed", entry.existed},
                      {"undoable", entry.undoable}});
    }
  }
  std::string error;
  CreatePrivateDirectories(directory_);
  AtomicWriteFile(directory_ + "/index.json", JsonDump({{"turns", index}}),
                  kPrivateFileMode, false, error);
}

}  // namespace uagent
