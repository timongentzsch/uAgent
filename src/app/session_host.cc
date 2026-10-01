// Copyright 2026 Timon Gentzsch

#include "include/app/session_host.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/agent/session_view.h"
#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/app/session.h"
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
HostSession::~HostSession() {
  stop.Wake();
  if (reader.joinable()) reader.join();
}

bool HostSession::Send(json frame) {
  std::lock_guard lock(send_mutex);
  StampFrame(frame, id, generation);
  return socket && WriteFrame(socket.Get(), frame);
}

SessionHost::SessionHost(std::string epoch, size_t byte_limit,
                         std::string executable, std::string directory,
                         size_t event_limit)
    : epoch_(std::move(epoch)),
      executable_(std::move(executable)),
      directory_(std::move(directory)),
      replay_(byte_limit, event_limit) {
  schedule_state_ = ReadSchedules();
  scheduled_view_ = ScheduleControl({{"action", "list"}});
}

void SessionHost::Publish(const std::string& session,
                          const std::string& generation, json value) {
  std::lock_guard lock(mutex_);
  auto owner = sessions_.find(session);
  const bool run_owned =
      owner != sessions_.end() && !owner->second->run_id.empty();
  replay_.Publish(epoch_, session, generation, std::move(value), run_owned);
  changed_.notify_all();
}

uint64_t SessionHost::Cursor() const {
  std::lock_guard lock(mutex_);
  return replay_.Cursor();
}

ReplayBatch SessionHost::ReadReplay(uint64_t next, bool valid,
                                    uint64_t watermark,
                                    size_t byte_budget) const {
  std::lock_guard lock(mutex_);
  return replay_.Read(next, valid, watermark, byte_budget);
}

void SessionHost::WaitForReplay(uint64_t cursor, std::chrono::seconds timeout) {
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, timeout,
                    [&] { return stopping_ || replay_.Cursor() > cursor; });
}

std::vector<HostNotice> SessionHost::WaitForNotices() {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] { return stopping_ || replay_.HasNotices(); });
  return replay_.TakeNotices();
}

void SessionHost::Stop() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
}

void SessionHost::LoadDrafts() {
  std::error_code ec;
  const std::string folder = directory_ + "/drafts";
  if (!EnsurePrivateDirectory(folder)) return;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (sessions_.size() >= kMaxCatalogueEntries) break;
    if (entry.path().extension() != ".json") continue;
    std::string bytes, error;
    if (!ReadRegularFile(entry.path().string(), kCatalogueHeaderBytes, bytes,
                         error)) {
      continue;
    }
    json draft = json::parse(bytes, nullptr, false);
    auto session = std::make_shared<HostSession>();
    session->id = JsonValue(draft, "id", "");
    session->path = JsonValue(draft, "path", "");
    session->cwd = JsonValue(draft, "cwd", "");
    const std::filesystem::path path(session->path);
    if (!OpaqueId(session->id) || session->id != HashHex(session->path) ||
        path.parent_path() != std::filesystem::path(UagentDir(kHistoryDir)) /
                                  WorkspaceId(session->cwd) ||
        !path.filename().string().starts_with("web-") ||
        path.extension() != ".json") {
      continue;
    }
    session->draft_title = JsonValue(draft, "title", "");
    session->title = session->draft_title.empty() ? "New conversation"
                                                  : session->draft_title;
    session->status = "draft";
    session->updated = JsonValue(draft, "updated", int64_t{0});
    sessions_[session->id] = std::move(session);
  }
}

bool SessionHost::PublishMetadata(const std::string& id, HostSession& session) {
  json after = Metadata(session);
  if (session.published == after) return false;
  session.published = after;
  replay_.Publish(epoch_, id, session.generation,
                  {{"kind", "metadata"}, {"metadata", std::move(after)}},
                  !session.run_id.empty());
  return true;
}

bool SessionHost::RefreshCatalogue(bool force) {
  std::lock_guard scan(scan_mutex_);
  if (!force &&
      std::chrono::steady_clock::now() - scanned_ < std::chrono::seconds(1)) {
    rescan_ = true;
    return false;
  }
  rescan_ = false;
  scanned_ = std::chrono::steady_clock::now();
  auto list = catalogue_.List(SessionScope::kAll);
  std::lock_guard lock(mutex_);
  bool changed = false;
  std::set<std::string> listed;
  for (const SessionInfo& item : list) {
    const std::string id = HashHex(item.path);
    listed.insert(id);
    auto [it, inserted] = sessions_.try_emplace(id, nullptr);
    if (inserted) {
      it->second = std::make_shared<HostSession>();
      it->second->id = id;
      it->second->path = item.path;
    }
    auto& session = *it->second;
    // A connecting session is read unlocked by its runtime's launch; the
    // next scan brings it up to date.
    if (session.closing || session.connecting || session.status == "updating" ||
        session.status == "deleting") {
      continue;
    }
    session.cwd = item.cwd;
    session.kind = item.kind;
    session.folder = JsonValue(item.thread, "folder", "");
    session.incoming = std::max(session.incoming, item.incoming);
    session.title = item.title;
    const FileStamp stamp = SnapshotFile(item.path);
    session.updated =
        stamp.modified_seconds * 1000 + stamp.modified_nanoseconds / 1000000;
    if (session.status == "draft") session.status = "saved";
    if (session.pid <= 0 || session.exited) session.error = item.error;
    if (inserted) session.published = nullptr;
    changed |= PublishMetadata(session.id, session);
  }
  // Gone from disk and not running: deleted by a coordinator or by hand.
  // Drafts have no file yet, and a session mid-command is the router's.
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    const HostSession& session = *it->second;
    if (listed.contains(it->first) || session.status == "draft" ||
        session.status == "updating" || session.status == "deleting" ||
        session.closing || session.connecting ||
        (session.pid > 0 && !session.exited) || PathExists(session.path)) {
      ++it;
      continue;
    }
    replay_.Publish(epoch_, it->first, "", {{"kind", "deleted"}},
                    !session.run_id.empty());
    it = sessions_.erase(it);
    changed = true;
  }
  // Waiting event streams send what was published at once.
  if (changed) changed_.notify_all();
  return changed;
}

std::optional<std::chrono::steady_clock::time_point> SessionHost::RescanDue()
    const {
  std::lock_guard scan(scan_mutex_);
  if (!rescan_) return std::nullopt;
  return scanned_ + std::chrono::seconds(1);
}

void SessionHost::RefreshPresence() {
  RefreshCatalogue();
  std::unique_lock lock(mutex_);
  std::vector<std::shared_ptr<HostSession>> candidates;
  for (const auto& [id, session] : sessions_) {
    if ((session->pid <= 0 || session->exited) && !session->closing &&
        session->status != "updating" && session->status != "deleting") {
      candidates.push_back(session);
    }
  }
  for (const auto& session : candidates) {
    if (!PathExists(SocketPath(session->path))) continue;
    if (session->parked && session->parked == session->updated) continue;
    std::string error;
    ActivateLocked(session, error, lock, false);
  }
}

void SessionHost::ParkIdleCoordinators() {
  std::lock_guard lock(mutex_);
  const int64_t now = NowMillis();
  const int64_t idle =
      std::chrono::duration_cast<std::chrono::milliseconds>(CoordinatorIdle())
          .count();
  for (const auto& [id, session] : sessions_) {
    if (session->kind != kSessionKindCoordinator || session->pid <= 0 ||
        session->exited || session->closing || session->connecting ||
        session->turn_active || !session->pending.is_null() ||
        now - std::max(session->updated, session->activated) < idle) {
      continue;
    }
    // Closing (not closed): the reader stops and the session reads as
    // saved, while the runtime decides for itself when to exit.
    session->parked = session->updated;
    session->closing = true;
    session->stop.Wake();
  }
}

json SessionHost::RestartRunning(const std::string& cwd,
                                 const std::string& only) {
  std::unique_lock lock(mutex_);
  std::vector<std::shared_ptr<HostSession>> idle;
  int64_t deferred = 0;
  for (const auto& [id, session] : sessions_) {
    if (session->pid <= 0 || session->exited || session->closing ||
        (!cwd.empty() && session->cwd != cwd) ||
        (!only.empty() && id != only)) {
      continue;
    }
    session->restart = true;
    if (session->turn_active) {
      ++deferred;
    } else {
      idle.push_back(session);
    }
  }
  for (const auto& session : idle) {
    std::string error;
    ActivateLocked(session, error, lock, false);
  }
  return {{"restarting", idle.size()}, {"deferred", deferred}};
}

void SessionHost::Shutdown() {
  std::map<std::string, std::shared_ptr<HostSession>> detached;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    changed_.notify_all();
    detached.swap(sessions_);
    for (const auto& [id, session] : detached) session->stop.Wake();
  }
  detached.clear();
}

bool SessionHost::Contains(const std::string& id) const {
  std::lock_guard lock(mutex_);
  return sessions_.contains(id);
}

json SessionHost::Metadata(const HostSession& session) const {
  // Waiting on a person: a coordinator's routed decision is not. What it
  // waits on is named from the decision in memory, for the inbox row.
  const bool waits = WaitsOnPerson(session.pending);
  return {
      {"id", session.id},
      {"task_id", session.task_id},
      {"run_id", session.run_id},
      {"cwd", session.cwd},
      {"kind", session.kind},
      {"folder", session.folder},
      {"title", session.title},
      {"generation", session.generation},
      {"status", session.status},
      {"presence", session.pid > 0 && !session.exited ? "active" : ""},
      {"turn_active", session.turn_active},
      {"guidance", session.guidance},
      {"incoming", session.incoming},
      {"activity", JsonValue(session.state, "activity", "Ready")},
      {"phase", JsonValue(session.state, "phase", "idle")},
      {"activities", JsonValue(session.state, "activities", json::array())},
      {"error", session.error},
      {"pending", waits},
      {"pending_kind", waits ? JsonValue(session.pending, "kind", "") : ""},
      {"pending_prompt", waits ? JsonValue(session.pending, "prompt", "") : ""},
      {"updated", session.updated}};
}

json SessionHost::LiveSnapshot(const HostSession& session) const {
  return {{"v", kProtocol},
          {"epoch", epoch_},
          {"cursor", replay_.Cursor()},
          {"metadata", Metadata(session)},
          {"state", session.state},
          {"pending", session.pending},
          {"live_truncated", session.live_truncated}};
}

json SessionHost::Catalogue() const {
  std::lock_guard lock(mutex_);
  json sessions = json::array();
  for (const auto& [id, session] : sessions_) {
    sessions.push_back(Metadata(*session));
  }
  return {{"cursor", replay_.Cursor()},
          {"sessions", std::move(sessions)},
          {"scheduled", scheduled_view_}};
}

}  // namespace uagent::session
