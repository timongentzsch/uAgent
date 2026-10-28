// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SANDBOX_WATCH_H_
#define UAGENT_INCLUDE_CORE_SANDBOX_WATCH_H_
// Where the sandbox cannot deny a write, it can still notice and undo one.
// Landlock has no deny form, so a repository's config and hooks and the
// folder's .mcp.json -- writable by being inside the folder -- are only
// recorded before a sandboxed command runs and put back when it ends.
//
// This is a check at the end of a command, not a wall: a change is visible to
// anything running meanwhile, and one made by someone else to the same paths
// while the command runs is undone with it.

#include <string>
#include <vector>

namespace uagent {

class SandboxWatch {
 public:
  // Records `paths` as they are now. A path that does not exist is recorded as
  // absent. A folder holding more than kLimit bytes or entries is not guarded.
  explicit SandboxWatch(std::vector<std::string> paths);

  // Puts back every recorded path that is different now; one notice per path,
  // naming it relative to `workspace` when it lies beneath it. Safe to call
  // again: what it restored is the record.
  std::vector<std::string> Restore(const std::string& workspace) const;

  struct Entry {
    // Relative to the watched path; empty for the path itself.
    std::string name;
    enum class Kind { kFile, kFolder, kLink } kind = Kind::kFile;
    std::string content;  // file bytes, or a link's target
    int mode = 0;
    bool operator==(const Entry&) const = default;
  };
  using Tree = std::vector<Entry>;  // empty: absent

 private:
  struct Watched {
    std::string path;
    Tree tree;
    bool guarded = true;
  };
  std::vector<Watched> watched_;
};

// The text to add to a finished command's output for what Restore put back,
// empty when nothing changed; each restoration is also a notice to the person.
// A null watch -- a host that denies the writes outright -- reports nothing.
std::string SandboxRestoreNote(const SandboxWatch* watch,
                               const std::string& workspace);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SANDBOX_WATCH_H_
