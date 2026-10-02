// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_EDIT_JOURNAL_H_
#define UAGENT_INCLUDE_AGENT_EDIT_JOURNAL_H_
// What each turn's file tools changed, kept so a person can put the files back.
// Per turn and file it keeps the bytes from before the turn's first change and
// a hash of what the turn left; a file is restored only while it still holds
// that, so an undo never overwrites a later change. Shell commands are not
// journaled: only the edit, write and delete tools report a FileEffect.
// Stored beside the session as <session>.json.edits/.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

class EditJournal {
 public:
  // Journals nothing until a directory is set; loads what it holds.
  void Open(std::string directory);
  // Where a session keeps what belongs to its edits; empty until opened.
  const std::string& Directory() const { return directory_; }
  void Record(int64_t turn, const FileEffect& effect);
  // [{path, added, removed, undoable}] for a turn summary, capped, and saves
  // the index. Empty for a turn that changed no file.
  json Files(int64_t turn);
  // The most recent turn that changed a file, 0 when none.
  int64_t LastTurn() const;
  // Puts `turn`'s files (or only `path`) back. {"restored": [paths],
  // "conflicts": [{path, reason}]}; restored files leave the journal.
  json Revert(int64_t turn, const std::string& path);

 private:
  struct Entry {
    std::string path, after_hash;
    bool existed = false, undoable = false;
  };
  std::string Blob(int64_t turn, const std::string& path) const;
  void Save() const;

  std::string directory_;
  std::map<int64_t, std::vector<Entry>> turns_;
  uint64_t stored_bytes_ = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_EDIT_JOURNAL_H_
