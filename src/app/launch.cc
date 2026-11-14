// Copyright 2026 Timon Gentzsch

#include "include/app/launch.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
std::string HistoryPath(const std::string& cwd, const std::string& name,
                        const std::string& host) {
  return UagentDir(kHistoryDir) + "/" + WorkspaceId(cwd, host) + "/" + name +
         ".json";
}

LaunchPaths PlanLaunch(const std::string& project, bool worktree,
                       const std::string& prefix, const std::string& id) {
  const std::string cwd =
      worktree ? UagentDir(kWorktreesDir) + "/" + id : project;
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

CapturedProcess WorkTreeDiff(const std::string& dir) {
  return HostGit(dir, {"diff", "--no-ext-diff", "--no-textconv", "--stat",
                       "--patch", "HEAD"});
}

std::string CreateWorktree(const std::string& project, const std::string& cwd) {
  auto created = HostGit(project, {"worktree", "add", "--detach", cwd, "HEAD"});
  if (created.Ok()) return "";
  return "Cannot create worktree: " +
         Utf8Prefix(created.output + created.error, 1024);
}

bool LaunchWorktree(const std::string& cwd) {
  // A saved cwd is the path the agent's process saw, which may name the
  // same folder another way (macOS's /var is /private/var).
  if (cwd.empty()) return false;
  const auto root = CanonicalAccessPath(UagentDir(kWorktreesDir));
  const auto path = CanonicalAccessPath(cwd);
  return path != root && PathWithin(path, root);
}

namespace {
// Why removing the launch worktree `cwd` would lose work, or empty.
std::string WorktreeKeepsWork(const std::string& cwd) {
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
  return "";
}
}  // namespace

std::string RemoveWorktree(const std::string& cwd) {
  if (!PathExists(cwd)) return "";
  if (std::string kept = WorktreeKeepsWork(cwd); !kept.empty()) {
    return kept;
  }
  // The repository is found from the worktree, which stays valid when the
  // conversation that started it has moved.
  auto common = HostGit(cwd, {"rev-parse", "--git-common-dir"});
  if (!common.Ok()) {
    return "cannot find the repository of " + cwd + ": " +
           Utf8Prefix(common.error, 512);
  }
  auto removed =
      HostGit((std::filesystem::path(cwd) / Trim(common.output)).string(),
              {"worktree", "remove", cwd});
  return removed.Ok() ? ""
                      : "Cannot remove worktree: " +
                            Utf8Prefix(removed.output + removed.error, 512);
}

std::string SendWhenReady(const session::Connection& connection,
                          const std::string& path, json command, bool idle) {
  constexpr int64_t kReadySeconds = 30;
  const std::string request = session::NewRequestId();
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
        session::StampFrame(command, SessionIdFor(path), connection.generation);
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
std::string DeleteConversation(const std::string& path,
                               const std::string& draft_path) {
  // Its agents breadth first, then reversed: an agent goes before the
  // conversation that started it, and the conversation itself goes last.
  const std::vector<SessionInfo> all = SessionsBeside(path);
  std::vector<std::string> ids = {SessionIdFor(path)};
  std::vector<SessionInfo> doomed;
  for (size_t at = 0; at < ids.size(); ++at) {
    for (const SessionInfo& info : all) {
      const std::string id = SessionIdFor(info.path);
      if (info.parent == ids[at] && std::ranges::find(ids, id) == ids.end()) {
        ids.push_back(id);
        doomed.push_back(info);
      }
    }
  }
  std::ranges::reverse(doomed);
  const auto named = [](const SessionInfo& info, const std::string& error) {
    return (info.parent.empty() ? "conversation " : "agent ") +
           SessionIdFor(info.path) + " \"" + OneLine(info.title) +
           "\": " + error;
  };
  for (const SessionInfo& info : doomed) {
    if (std::string error = CloseRuntime(info.path); !error.empty()) {
      return named(info, error);
    }
  }
  const auto self = std::ranges::find_if(all, [&](const SessionInfo& info) {
    return SessionIdFor(info.path) == ids.front();
  });
  SessionInfo own;
  own.path = path;
  doomed.push_back(self != all.end() ? *self : own);
  // Every worktree is looked at before anything goes, so a refusal leaves
  // the whole tree as it was.
  for (const SessionInfo& info : doomed) {
    if (!info.host.empty() || !LaunchWorktree(info.cwd) ||
        !PathExists(info.cwd)) {
      continue;
    }
    if (std::string kept = WorktreeKeepsWork(info.cwd); !kept.empty()) {
      return named(info, kept);
    }
  }
  for (const SessionInfo& info : doomed) {
    if (info.host.empty() && LaunchWorktree(info.cwd)) {
      if (std::string kept = RemoveWorktree(info.cwd); !kept.empty()) {
        return named(info, kept);
      }
    }
    const SessionStoreStatus removed = SessionStore::Remove(
        info.path, info.path == path ? draft_path : std::string());
    if (!removed.Ok()) return named(info, removed.message);
  }
  return "";
}

std::string RuntimeStatus(const std::string& path) {
  session::Connection connection = session::Connect(path);
  if (!connection.socket) return "saved";
  if (!connection.status.empty()) return connection.status;
  // One still starting has said nothing yet: its first state says it.
  std::string status = "idle";
  session::ReadFrames(
      connection.socket.Get(), -1, session::kFrameBytes,
      [&](const json& frame) {
        if (JsonValue(frame, "kind", "") != "state") return true;
        status = Standing(JsonValue(frame, "pending", json()),
                          JsonValue(frame, "busy", false));
        return false;
      },
      // A live runtime answers at once; a stuck one must not hold up who
      // asks.
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500));
  return status;
}
}  // namespace uagent
