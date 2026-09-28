// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_LAUNCH_H_
#define UAGENT_INCLUDE_APP_LAUNCH_H_
#include <string>

#include "include/app/session.h"
#include "include/core/json.h"

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

// Sends one command to the runtime of `path` once it has published its state
// (and, with `idle`, once no turn is running). Returns the error, or empty.
std::string SendWhenReady(const session::Connection& connection,
                          const std::string& path, json command, bool idle);
}  // namespace uagent
#endif
