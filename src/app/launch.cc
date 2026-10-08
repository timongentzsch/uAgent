// Copyright 2026 Timon Gentzsch

#include "include/app/launch.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
std::string HistoryPath(const std::string& cwd, const std::string& name) {
  return UagentDir(kHistoryDir) + "/" + WorkspaceId(cwd) + "/" + name + ".json";
}

LaunchPaths PlanLaunch(const std::string& project, bool worktree,
                       const std::string& prefix, const std::string& id) {
  const std::string cwd =
      worktree ? UagentDir("worktrees") + "/" + id : project;
  return {cwd, HistoryPath(cwd, prefix + id)};
}

CapturedProcess HostGit(const std::string& dir, std::vector<std::string> args) {
  std::vector<std::string> argv = {"git",
                                   "-C",
                                   dir,
                                   "-c",
                                   "core.hooksPath=/dev/null",
                                   "-c",
                                   "core.fsmonitor=false"};
  argv.insert(argv.end(), args.begin(), args.end());
  return CaptureProcess(argv, 30);
}

std::string CreateWorktree(const std::string& project, const std::string& cwd) {
  auto created = HostGit(project, {"worktree", "add", "--detach", cwd, "HEAD"});
  if (created.Ok()) return "";
  return "Cannot create worktree: " +
         Utf8Prefix(created.output + created.error, 1024);
}

bool LaunchWorktree(const std::string& cwd) {
  return cwd.starts_with(UagentDir("worktrees") + "/");
}

std::string RemoveWorktree(const std::string& project, const std::string& cwd) {
  if (!PathExists(cwd)) return "";
  auto changes = HostGit(cwd, {"status", "--porcelain"});
  auto loose =
      HostGit(cwd, {"rev-list", "HEAD", "--not", "--branches", "--tags"});
  if (!changes.Ok() || !loose.Ok()) {
    return "cannot inspect " + cwd + ": " +
           Utf8Prefix(changes.error + loose.error, 512);
  }
  if (!Trim(changes.output).empty()) {
    return "uncommitted changes in " + cwd + "; merge or discard them first";
  }
  if (!Trim(loose.output).empty()) {
    return "commits in " + cwd +
           " are on no branch; merge or branch them "
           "first";
  }
  auto removed = HostGit(project, {"worktree", "remove", cwd});
  return removed.Ok() ? ""
                      : "Cannot remove worktree: " +
                            Utf8Prefix(removed.output + removed.error, 512);
}

std::string SendWhenReady(const session::Connection& connection,
                          const std::string& path, json command, bool idle) {
  constexpr int64_t kReadySeconds = 30;
  const std::string request = session::RandomToken(16);
  bool sent = false;
  std::string error = "the session runtime did not become ready";
  session::ReadFrames(
      connection.socket.Get(), -1, session::kFrameBytes,
      [&](const json& frame) {
        const std::string kind = JsonValue(frame, "kind", "");
        if (sent) {
          // The runtime's verdict on this command: accepted, or why not.
          if (kind != "outcome" ||
              JsonValue(frame, "request_id", "") != request) {
            return true;
          }
          error = JsonValue(frame, "accepted", false)
                      ? ""
                      : JsonValue(frame, "error", "command refused");
          return false;
        }
        if (kind != "state" || (idle && JsonValue(frame, "busy", true))) {
          return true;
        }
        session::StampFrame(command, HashHex(path), connection.generation);
        command["request_id"] = request;
        sent = session::WriteFrame(connection.socket.Get(), command);
        return sent;
      },
      DeadlineAfter(kReadySeconds));
  return error;
}

std::optional<std::string> SendToRunning(const std::string& path,
                                         json command) {
  const session::Connection connection = session::Connect(path);
  if (!connection.socket) return std::nullopt;
  return SendWhenReady(connection, path, std::move(command), false);
}

// It acknowledges the close, then exits, which closes the socket. Liveness is
// the handshake, never the socket file, which a crashed runtime leaves behind.
std::string CloseRuntime(const std::string& path) {
  session::Connection connection = session::Connect(path);
  if (!connection.socket) return "";
  std::string error =
      SendWhenReady(connection, path, {{"kind", "close"}}, false);
  if (!error.empty()) return error;
  session::ReadFrames(
      connection.socket.Get(), -1, session::kFrameBytes,
      [](const json&) { return true; },
      std::chrono::steady_clock::now() + session::kWorkerShutdownTimeout);
  return session::Connect(path).socket ? "the session did not exit in time"
                                       : "";
}
}  // namespace uagent
