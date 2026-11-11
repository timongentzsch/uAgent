// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_LAUNCH_H_
#define UAGENT_INCLUDE_APP_LAUNCH_H_
// Where a launched session runs and where its file lives, and how to reach
// its runtime. Used by scheduled runs and agents.

#include <optional>
#include <string>
#include <vector>

#include "include/app/session.h"
#include "include/core/capture.h"
#include "include/core/json.h"

namespace uagent {
struct LaunchPaths {
  std::string cwd, path;
};

// The session file `name`.json in the history of folder `cwd`.
std::string HistoryPath(const std::string& cwd, const std::string& name,
                        const std::string& host = {});

// With `worktree`, a fresh detached worktree under ~/.uagent/worktrees/<id>,
// else `project` itself. The file is history/<folder-hash>/<prefix><id>.
LaunchPaths PlanLaunch(const std::string& project, bool worktree,
                       const std::string& prefix, const std::string& id);

// Runs git in `dir` for the host with hooks, fsmonitor, external diff and
// textconv off: a sandboxed session may have written the repository config.
CapturedProcess HostGit(const std::string& dir, std::vector<std::string> args);
// What `dir`'s work tree changed against HEAD, as a patch with its summary.
CapturedProcess WorkTreeDiff(const std::string& dir);

// Creates the detached worktree `cwd` at the folder's HEAD. Returns the error.
std::string CreateWorktree(const std::string& project, const std::string& cwd);

// Whether `cwd` is a worktree PlanLaunch made.
bool LaunchWorktree(const std::string& cwd);

// Removes the launch worktree `cwd` unless it holds uncommitted changes or
// commits outside a branch or tag. Returns why not, or empty once it is gone.
std::string RemoveWorktree(const std::string& cwd);

// Sends one command to the runtime of `path` once it has published its state
// (with `idle`: once no turn runs). Returns its refusal or a transport error.
std::string SendWhenReady(const session::Connection& connection,
                          const std::string& path, json command, bool idle);
// SendWhenReady to the runtime of `path`, busy or not; nullopt when none runs.
std::optional<std::string> SendToRunning(const std::string& path, json command);
// Closes the runtime of `path` and waits. Returns why it still runs, or empty.
std::string CloseRuntime(const std::string& path);
// Deletes the conversation at `path` (runtime already closed) and its agents,
// deepest first; nothing when a worktree holds work that would be lost.
std::string DeleteConversation(const std::string& path,
                               const std::string& draft_path = "");
// "saved" without a runtime, else "needs you", working or idle. A decision
// still with a parent counts as working.
std::string RuntimeStatus(const std::string& path);
}  // namespace uagent
#endif
