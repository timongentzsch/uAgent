// Copyright 2026 Timon Gentzsch

#include "include/app/launch.h"

#include <string>

#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {
LaunchPaths PlanLaunch(const std::string& project, bool worktree,
                       const std::string& prefix, const std::string& id) {
  const std::string cwd =
      worktree ? UagentDir("worktrees") + "/" + id : project;
  return {cwd, UagentDir(kHistoryDir) + "/" + WorkspaceId(cwd) + "/" + prefix +
                   id + ".json"};
}

std::string CreateWorktree(const std::string& project, const std::string& cwd) {
  auto created = CaptureProcess(
      {"git", "-C", project, "worktree", "add", "--detach", cwd, "HEAD"}, 30);
  if (created.Ok()) return "";
  return "Cannot create worktree: " +
         Utf8Prefix(created.output + created.error, 1024);
}
}  // namespace uagent
