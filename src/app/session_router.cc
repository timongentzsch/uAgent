// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_view.h"
#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/app/session.h"
#include "include/app/session_command.h"
#include "include/app/session_host.h"
#include "include/cli.h"
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
SessionCommandResult SessionHost::ExecuteCommand(
    json command, const std::string& device, const std::string& request_id) {
  std::unique_lock lock(mutex_);
  SessionCommandResult result;
  result.outcome = {{"request_id", request_id}, {"accepted", true}};
  const SessionCommandKind kind =
      ParseSessionCommandKind(JsonValue(command, "kind", ""));
  if (kind == SessionCommandKind::kCreate) {
    std::error_code ec;
    auto cwd = std::filesystem::canonical(JsonValue(command, "cwd", ""), ec);
    if (ec || !std::filesystem::is_directory(cwd, ec)) {
      result.error = "choose an accessible directory on the host";
      return result;
    }
    auto path = UagentDir(kHistoryDir) + "/" + WorkspaceId(cwd.string()) +
                "/web-" + RandomToken(16) + ".json";
    auto session = CreateSession(cwd.string(), path, "", result.error);
    if (session) {
      result.wake = true;
      result.outcome["session"] = Metadata(*session);
    }
    return result;
  }
  auto found = sessions_.find(JsonValue(command, "session_id", ""));
  if (found == sessions_.end()) {
    result.error = "unknown session";
    return result;
  }
  auto session = found->second;
  if (session->closing || session->connecting ||
      session->status == "updating" || session->status == "deleting") {
    result.error = "conversation update in progress";
    return result;
  }
  if ((kind == SessionCommandKind::kFork ||
       kind == SessionCommandKind::kRewind) &&
      command.contains("argument")) {
    // Browser clients send the typed argument; the grammar lives natively.
    // Rewind shares /fork's [@]TURN and takes no title.
    const ForkArgument parsed =
        ParseForkArgument(JsonValue(command, "argument", ""));
    command.erase("argument");
    const bool fork = kind == SessionCommandKind::kFork;
    if (fork) command["title"] = parsed.title;
    command["turn"] = fork || parsed.title.empty() ? parsed.turn : 0;
  }
  if (kind == SessionCommandKind::kFork && session->pid <= 0) {
    if (JsonValue(command, "generation", "") != session->generation) {
      result.error = "stale session; refresh before acting";
    } else if (sessions_.size() >= kMaxCatalogueEntries) {
      result.error = "session catalogue limit reached";
    } else {
      lock.unlock();
      json fork =
          SessionStore::Fork(session->path, JsonValue(command, "title", ""),
                             false, JsonValue(command, "turn", int64_t{0}));
      if (RefreshCatalogue(true)) changed_.notify_all();
      lock.lock();
      result.outcome["result"] = fork;
      result.error = JsonValue(fork, "error", "");
    }
    return result;
  }
  if ((kind == SessionCommandKind::kRename ||
       kind == SessionCommandKind::kDelete) &&
      session->pid <= 0) {
    if (JsonValue(command, "generation", "") != session->generation) {
      result.error = "stale session; refresh before acting";
      return result;
    }
    std::string title = Trim(JsonValue(command, "title", ""));
    if (kind == SessionCommandKind::kRename && !ValidSessionTitle(title)) {
      result.error = "title must be 1–256 bytes without control characters";
      return result;
    }
    std::string prior_status = session->status;
    session->status =
        kind == SessionCommandKind::kDelete ? "deleting" : "updating";
    lock.unlock();
    std::unique_lock scan(scan_mutex_);
    std::unique_lock asset_lock = assets_.GuardMutation();
    std::string draft_path = directory_ + "/drafts/" + session->id + ".json";
    SessionStoreStatus stored;
    if (kind == SessionCommandKind::kDelete) {
      stored = SessionStore::Remove(session->path, draft_path);
    } else if (PathExists(session->path)) {
      stored = SessionStore::Rename(session->path, title);
    } else {
      const int64_t updated = NowMillis();
      auto written =
          ToolWritePrivateFile(draft_path, JsonDump({{"id", session->id},
                                                     {"path", session->path},
                                                     {"cwd", session->cwd},
                                                     {"title", title},
                                                     {"updated", updated}}));
      if (!written.Ok()) result.error = "cannot save conversation title";
    }
    if (!stored.Ok()) result.error = stored.message;
    asset_lock.unlock();
    if (result.error.empty() && kind == SessionCommandKind::kDelete) {
      assets_.Invalidate();
    }
    lock.lock();
    session->status = prior_status;
    if (result.error.empty() && kind == SessionCommandKind::kDelete) {
      sessions_.erase(session->id);
      replay_.Publish(epoch_, session->id, "", {{"kind", "deleted"}},
                      !session->run_id.empty());
    } else if (result.error.empty()) {
      session->title = title;
      session->draft_title = title;
      session->updated =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      replay_.Publish(epoch_, session->id, "",
                      {{"kind", "metadata"}, {"metadata", Metadata(*session)}},
                      !session->run_id.empty());
    }
    return result;
  }
  if (kind == SessionCommandKind::kDelete) {
    result.error = "stop and close this conversation before deleting it";
  } else if (kind == SessionCommandKind::kActivate) {
    if (ActivateLocked(session, result.error, lock, true)) {
      result.outcome["session"] = Metadata(*session);
    }
  } else if (JsonValue(command, "generation", "") != session->generation ||
             session->generation.empty()) {
    result.error = "stale worker generation; refresh before acting";
  } else if (kind == SessionCommandKind::kClose) {
    auto recorded = session->run_id.empty()
                        ? json::object()
                        : UpdateScheduledRun(session->run_id, "interrupted",
                                             "Session closed by user.");
    if (recorded.contains("error")) {
      result.error = JsonValue(recorded, "error", "cannot record interruption");
    } else if (session->exited) {
      DeactivateLocked(*session);
    } else {
      session->closing = true;
      session->status = "closing";
      if (PublishMetadata(session->id, *session)) changed_.notify_all();
      result.wake = true;
      lock.unlock();
      session->Send({{"kind", "close"}, {"request_id", request_id}});
      lock.lock();
      changed_.wait_for(lock, std::chrono::seconds(5),
                        [&] { return session->exited.load(); });
    }
  } else if (session->pid <= 0 || session->exited) {
    result.error = "session needs activation";
  } else if (ForwardsToWorker(kind)) {
    command["attachments"] = json::array();
    AssetClaim claim;
    if (outcomes_.PendingCommands(*session) >= kMaxPendingSessionCommands) {
      result.error = "too many unacknowledged worker commands";
    }
    if (const json* ids = JsonArray(command, "attachment_ids");
        result.error.empty() && ids && !ids->empty()) {
      lock.unlock();
      result.error = assets_.Claim(
          session->path, *ids, command,
          [&] {
            std::lock_guard guard(mutex_);
            if (!sessions_.contains(session->id) ||
                sessions_.at(session->id) != session || session->exited ||
                session->status == "deleting") {
              return std::string(
                  "session changed while claiming attachments; refresh");
            }
            return std::string();
          },
          claim);
      lock.lock();
    }
    if (result.error.empty() && JsonDump(command).size() > kCommandBytes) {
      result.error =
          "message and resolved attachments exceed the command limit";
    }
    if (result.error.empty()) {
      result.worker_request = HashHex(device) + HashHex(request_id);
      command["client_request_id"] = kind == SessionCommandKind::kRecall
                                         ? JsonValue(command, "target_id", "")
                                         : request_id;
      lock.unlock();
      bool dispatched = false;
      result.outcome =
          outcomes_.SendCommand(session, std::move(command),
                                result.worker_request, request_id, dispatched);
      lock.lock();
      if (!JsonValue(result.outcome, "accepted", false)) {
        result.error = JsonValue(result.outcome, "error", "command rejected");
        if (!dispatched && !claim.records.empty()) {
          lock.unlock();
          assets_.Unclaim(claim);
          lock.lock();
        }
      }
    }
  } else {
    result.error = "unsupported command";
  }
  return result;
}

}  // namespace uagent::session
