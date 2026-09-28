// Copyright 2026 Timon Gentzsch

#include "include/app/launch.h"

#include <string>

#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"

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

std::string SendWhenReady(const session::Connection& connection,
                          const std::string& path, json command, bool idle) {
  constexpr int64_t kReadySeconds = 30;
  session::Pipe never;
  if (!never.Open()) return "cannot wait for the session runtime";
  bool sent = false;
  session::ReadFrames(
      connection.socket.Get(), never.read.Get(), session::kFrameBytes,
      [&](const json& frame) {
        if (JsonValue(frame, "kind", "") != "state" ||
            (idle && JsonValue(frame, "busy", true))) {
          return true;
        }
        command["v"] = session::kProtocol;
        command["session_id"] = HashHex(path);
        command["generation"] = connection.generation;
        command["request_id"] = session::RandomToken(16);
        sent = session::WriteFrame(connection.socket.Get(), command);
        return false;
      },
      DeadlineAfter(kReadySeconds));
  return sent ? "" : "the session runtime did not become ready";
}
}  // namespace uagent
