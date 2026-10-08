// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_LAUNCH_H_
#define UAGENT_INCLUDE_APP_LAUNCH_H_
#include <optional>
#include <string>
#include <vector>

#include "include/app/session.h"
#include "include/core/capture.h"
#include "include/core/json.h"

namespace uagent {
// Where a launched session runs and where its file lives. Shared by scheduled
// runs and coordinator threads so both isolate work the same way.
struct LaunchPaths {
  std::string cwd, path;
};

// The session file `name`.json in the history of workspace `cwd`.
std::string HistoryPath(const std::string& cwd, const std::string& name);

// `worktree` runs in a fresh detached worktree under ~/.uagent/worktrees/<id>,
// otherwise in `project` itself. The file is history/<workspace>/<prefix><id>.
LaunchPaths PlanLaunch(const std::string& project, bool worktree,
                       const std::string& prefix, const std::string& id);

// Runs git in `dir` for the host. The directory may be one a sandboxed
// session could write, so repository config never makes git run a command:
// no hooks, fsmonitor, external diff or textconv.
CapturedProcess HostGit(const std::string& dir, std::vector<std::string> args);

// Creates the detached worktree `cwd` at the project's HEAD. Returns the
// error, or empty on success.
std::string CreateWorktree(const std::string& project, const std::string& cwd);

// Whether `cwd` is a worktree PlanLaunch made.
bool LaunchWorktree(const std::string& cwd);

// Removes the launch worktree `cwd` of `project` if nothing in it would be
// lost: no uncommitted changes and no commits outside a branch or tag.
// Returns why not, or empty once it is gone.
std::string RemoveWorktree(const std::string& project, const std::string& cwd);

// Sends one command to the runtime of `path` once it has published its state
// (and, with `idle`, once no turn is running), and waits for its outcome.
// Returns the runtime's refusal or a transport error, or empty.
std::string SendWhenReady(const session::Connection& connection,
                          const std::string& path, json command, bool idle);
// SendWhenReady to the runtime of `path`, busy or not; nullopt when none runs.
std::optional<std::string> SendToRunning(const std::string& path, json command);
// Closes the runtime of `path` and waits until it is gone. Returns why it
// still runs, or empty once nothing does (also when nothing did).
std::string CloseRuntime(const std::string& path);
}  // namespace uagent
#endif
