// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_LAUNCH_H_
#define UAGENT_INCLUDE_APP_LAUNCH_H_
#include <string>

namespace uagent {
// Where a launched session runs and where its file lives. Shared by scheduled
// runs and coordinator threads so both isolate work the same way.
struct LaunchPaths {
  std::string cwd, path;
};

// `worktree` runs in a fresh detached worktree under ~/.uagent/worktrees/<id>,
// otherwise in `project` itself. The file is history/<workspace>/<prefix><id>.
LaunchPaths PlanLaunch(const std::string& project, bool worktree,
                       const std::string& prefix, const std::string& id);

// Creates the detached worktree `cwd` at the project's HEAD. Returns the
// error, or empty on success.
std::string CreateWorktree(const std::string& project, const std::string& cwd);
}  // namespace uagent
#endif
