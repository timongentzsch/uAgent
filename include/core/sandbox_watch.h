// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SANDBOX_WATCH_H_
#define UAGENT_INCLUDE_CORE_SANDBOX_WATCH_H_
// Landlock has no deny form, so paths that must not change are recorded before
// a sandboxed command and put back when it ends: a check, not a wall.

#include <string>
#include <vector>

namespace uagent {

class SandboxWatch {
 public:
  // A missing path is recorded as absent. A folder over the watch limits
  // (bytes or entries) is not guarded.
  explicit SandboxWatch(std::vector<std::string> paths);

  // Puts back every recorded path that differs now and returns one notice per
  // path. Safe to call again.
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

// The text a finished command's output gains for what Restore put back; empty
// when nothing changed or `watch` is null.
std::string SandboxRestoreNote(const SandboxWatch* watch,
                               const std::string& workspace);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SANDBOX_WATCH_H_
