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
#include "include/app/session_host.h"
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
namespace {
// Attachment display names travel in frames and land on disk-adjacent
// records: no path separators, no control bytes, bounded length.
bool ValidAssetName(const std::string& name) {
  if (name.size() > 128 || name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    return false;
  }
  for (char ch : name) {
    const auto code = static_cast<unsigned char>(ch);
    if (code < 32 || code == 127) return false;
  }
  return true;
}

void UnclaimAttachments(std::vector<std::pair<std::string, json>>& claims,
                        size_t count) {
  for (size_t i = 0; i < count && i < claims.size(); ++i) {
    claims[i].second["committed"] = false;
    ToolWritePrivateFile(claims[i].first, JsonDump(claims[i].second));
  }
}

}  // namespace
json SessionHost::SendCommand(const std::shared_ptr<HostSession>& session,
                              json command, const std::string& worker_request,
                              const std::string& client_request,
                              bool& dispatched) {
  dispatched = false;
  {
    std::lock_guard lock(outcome_mutex_);
    if (outcomes_.size() >= 512) {
      auto completed = std::find_if(
          outcomes_.begin(), outcomes_.end(), [](const auto& entry) {
            return !JsonValue(entry.second, "pending", false);
          });
      if (completed == outcomes_.end()) {
        return {{"request_id", client_request},
                {"accepted", false},
                {"error", "too many unacknowledged worker commands"}};
      }
      outcomes_.erase(completed);
    }
    outcomes_[worker_request] = {
        {"request_id", client_request}, {"accepted", true}, {"pending", true}};
    session->awaiting.push_back(worker_request);
  }
  command["request_id"] = worker_request;
  if (!session->Send(std::move(command))) {
    std::lock_guard lock(outcome_mutex_);
    std::erase(session->awaiting, worker_request);
    return outcomes_[worker_request] = {{"request_id", client_request},
                                        {"accepted", false},
                                        {"error", "worker is disconnected"}};
  }
  dispatched = true;
  std::unique_lock lock(outcome_mutex_);
  outcome_changed_.wait_for(lock, std::chrono::seconds(2), [&] {
    return !JsonValue(outcomes_.at(worker_request), "pending", false) ||
           session->exited;
  });
  return outcomes_.at(worker_request);
}

json SessionHost::CommandOutcome(const std::string& worker_request,
                                 const std::string& client_request) const {
  std::lock_guard lock(outcome_mutex_);
  auto found = outcomes_.find(worker_request);
  return found == outcomes_.end()
             ? json{{"request_id", client_request}, {"unknown", true}}
             : found->second;
}

size_t SessionHost::PendingCommands(const HostSession& session) const {
  std::lock_guard lock(outcome_mutex_);
  return session.awaiting.size();
}

bool SessionHost::ResolveOutcome(HostSession& session, json& frame) {
  const std::string worker_request = JsonValue(frame, "request_id", "");
  std::lock_guard lock(outcome_mutex_);
  auto found = outcomes_.find(worker_request);
  if (found == outcomes_.end()) return false;
  const std::string client_request = JsonValue(found->second, "request_id", "");
  frame["request_id"] = client_request;
  found->second = {{"request_id", client_request},
                   {"accepted", JsonValue(frame, "accepted", false)},
                   {"pending", JsonValue(frame, "pending", false)},
                   {"error", JsonValue(frame, "error", "")},
                   {"result", JsonValue(frame, "result", json::object())}};
  if (!JsonValue(frame, "pending", false)) {
    std::erase(session.awaiting, worker_request);
  }
  outcome_changed_.notify_all();
  return true;
}

void SessionHost::FailPending(HostSession& session) {
  std::lock_guard lock(outcome_mutex_);
  for (const std::string& worker_request : session.awaiting) {
    auto found = outcomes_.find(worker_request);
    if (found == outcomes_.end()) continue;
    found->second = {{"request_id", JsonValue(found->second, "request_id", "")},
                     {"accepted", false},
                     {"error",
                      "worker closed before acknowledgement; inspect history "
                      "before retrying"}};
  }
  session.awaiting.clear();
  outcome_changed_.notify_all();
}

SessionCommandResult SessionHost::ExecuteCommand(
    json command, const std::string& device, const std::string& request_id) {
  std::unique_lock lock(mutex_);
  SessionCommandResult result;
  result.outcome = {{"request_id", request_id}, {"accepted", true}};
  const std::string kind = JsonValue(command, "kind", "");
  if (kind == "create") {
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
  if (kind == "fork" && session->pid <= 0) {
    if (JsonValue(command, "generation", "") != session->generation) {
      result.error = "stale session; refresh before acting";
    } else if (sessions_.size() >= 4096) {
      result.error = "session catalogue limit reached";
    } else {
      lock.unlock();
      json fork =
          SessionStore::Fork(session->path, JsonValue(command, "title", ""));
      if (RefreshCatalogue(true)) changed_.notify_all();
      lock.lock();
      result.outcome["result"] = fork;
      result.error = JsonValue(fork, "error", "");
    }
    return result;
  }
  if ((kind == "rename" || kind == "delete") && session->pid <= 0) {
    if (JsonValue(command, "generation", "") != session->generation) {
      result.error = "stale session; refresh before acting";
      return result;
    }
    std::string title = Trim(JsonValue(command, "title", ""));
    if (kind == "rename" && !ValidSessionTitle(title)) {
      result.error = "title must be 1–256 bytes without control characters";
      return result;
    }
    std::string prior_status = session->status;
    session->status = kind == "delete" ? "deleting" : "updating";
    lock.unlock();
    std::unique_lock scan(scan_mutex_);
    std::unique_lock asset_lock(asset_mutex_);
    std::string draft_path = directory_ + "/drafts/" + session->id + ".json";
    SessionStoreStatus stored;
    if (kind == "delete") {
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
    lock.lock();
    session->status = prior_status;
    if (result.error.empty() && kind == "delete") {
      sessions_.erase(session->id);
      assets_scanned_ = {};
      PublishLocked(session->id, "", {{"kind", "deleted"}});
    } else if (result.error.empty()) {
      session->title = title;
      session->draft_title = title;
      session->updated =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      PublishLocked(session->id, "",
                    {{"kind", "metadata"}, {"metadata", Metadata(*session)}});
    }
    return result;
  }
  if (kind == "delete") {
    result.error = "stop and close this conversation before deleting it";
  } else if (kind == "activate") {
    if (ActivateLocked(session, result.error, lock, true)) {
      result.outcome["session"] = Metadata(*session);
    }
  } else if (JsonValue(command, "generation", "") != session->generation ||
             session->generation.empty()) {
    result.error = "stale worker generation; refresh before acting";
  } else if (kind == "close") {
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
  } else if (kind == "submit" || kind == "steer" || kind == "interrupt" ||
             kind == "recall" || kind == "reply" || kind == "refresh" ||
             kind == "rename" || kind == "model" || kind == "activity" ||
             kind == "permissions" || kind == "config" || kind == "context" ||
             kind == "fork" || kind == "prompt") {
    command["attachments"] = json::array();
    std::vector<std::pair<std::string, json>> claims;
    if (PendingCommands(*session) >= 32) {
      result.error = "too many unacknowledged worker commands";
    }
    if (const json* ids = JsonArray(command, "attachment_ids");
        result.error.empty() && ids && !ids->empty()) {
      lock.unlock();
      std::unique_lock asset_lock(asset_mutex_);
      if (ids->size() > kUploadCount) result.error = "too many attachments";
      if (!EnsurePrivateDirectory(session->path + ".assets")) {
        result.error = "unsafe asset directory";
      }
      for (const json& claim : *ids) {
        if (!result.error.empty()) break;
        // Claims are bare asset IDs, or {id, name} objects carrying the
        // composer's display name (deduped/renamed labels). The display
        // name is stored on the record before commit, so the history echo
        // and the model payload agree with what the sender saw.
        std::string asset_id = claim.is_string() ? claim.get<std::string>()
                                                 : JsonValue(claim, "id", "");
        std::string display =
            claim.is_object() ? JsonValue(claim, "name", "") : "";
        if (!OpaqueId(asset_id)) {
          result.error = "invalid asset ID";
          break;
        }
        if (!display.empty() && !ValidAssetName(display)) {
          result.error = "invalid attachment name";
          break;
        }
        std::string stem = session->path + ".assets/" + asset_id;
        std::string metadata, read_error;
        if (!ReadRegularFile(stem + ".json", 1024, metadata, read_error)) {
          result.error = "attachment is unavailable";
          break;
        }
        json asset = json::parse(metadata, nullptr, false);
        std::string extension = JsonValue(
            asset, "extension", ImageExtension(JsonValue(asset, "mime", "")));
        if (extension.empty() ||
            (extension != ".data" &&
             extension != ImageExtension(JsonValue(asset, "mime", "")))) {
          result.error = "invalid file asset";
          break;
        }
        if (!display.empty()) asset["name"] = display;
        claims.emplace_back(stem + ".json", asset);
        command["attachments"].push_back(
            {{"path", stem + extension},
             {"id", asset_id},
             {"name", JsonValue(asset, "name", "attachment" + extension)},
             {"mime", JsonValue(asset, "mime", "application/octet-stream")},
             {"image", JsonValue(asset, "image", extension != ".data")}});
      }
      lock.lock();
      if (!sessions_.contains(session->id) ||
          sessions_.at(session->id) != session || session->exited) {
        result.error = "session changed while claiming attachments; refresh";
      }
      if (result.error.empty() && JsonDump(command).size() > kCommandBytes) {
        result.error =
            "message and resolved attachments exceed the command limit";
      }
      if (result.error.empty()) {
        size_t committed = 0;
        for (auto& [path, asset] : claims) {
          asset["committed"] = true;
          if (!ToolWritePrivateFile(path, JsonDump(asset)).Ok()) {
            result.error = "cannot claim attachment";
            break;
          }
          ++committed;
        }
        if (!result.error.empty()) {
          UnclaimAttachments(claims, committed);
        }
      }
      asset_lock.unlock();
    }
    if (result.error.empty() && JsonDump(command).size() > kCommandBytes) {
      result.error =
          "message and resolved attachments exceed the command limit";
    }
    if (result.error.empty()) {
      result.worker_request = HashHex(device) + HashHex(request_id);
      command["client_request_id"] =
          kind == "recall" ? JsonValue(command, "target_id", "") : request_id;
      lock.unlock();
      bool dispatched = false;
      result.outcome =
          SendCommand(session, std::move(command), result.worker_request,
                      request_id, dispatched);
      lock.lock();
      if (!JsonValue(result.outcome, "accepted", false)) {
        result.error = JsonValue(result.outcome, "error", "command rejected");
        if (!dispatched && !claims.empty()) {
          lock.unlock();
          std::lock_guard asset_lock(asset_mutex_);
          UnclaimAttachments(claims, claims.size());
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
