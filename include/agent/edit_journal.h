// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_EDIT_JOURNAL_H_
#define UAGENT_INCLUDE_AGENT_EDIT_JOURNAL_H_
// What each turn's edit, write and delete tools changed, kept for undo. A file
// is restored only while it still holds what the turn left.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// Restores only while `path` still hashes to `after_hash`; why not, or empty.
std::string RestoreFile(const std::string& path, const std::string& after_hash,
                        bool existed, const std::string& before, uint32_t mode);

class EditJournal {
 public:
  // Journals nothing until a directory is set; loads what it holds.
  void Open(std::string directory);
  // Empty until opened.
  const std::string& Directory() const { return directory_; }
  void Record(int64_t turn, const FileEffect& effect);
  // [{path, added, removed, undoable}], capped; also saves the index.
  json Files(int64_t turn);
  // The most recent turn that changed a file, 0 when none.
  int64_t LastTurn() const;
  // {restored: [paths], conflicts: [{path, reason}]}; an empty `path` is all.
  json Revert(int64_t turn, const std::string& path);

 private:
  struct Entry {
    std::string path, after_hash;
    bool existed = false, undoable = false;
    std::optional<uint32_t> before_mode = std::nullopt;
  };
  std::string Blob(int64_t turn, const std::string& path) const;
  void Save() const;

  std::string directory_;
  std::map<int64_t, std::vector<Entry>> turns_;
  uint64_t stored_bytes_ = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_EDIT_JOURNAL_H_
