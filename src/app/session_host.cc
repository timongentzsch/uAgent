// Copyright 2026 Timon Gentzsch

#include "include/app/session_host.h"

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
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
namespace {
struct AssetUsage {
  size_t bytes = 0, count = 0;
  bool valid = true;
};

AssetUsage InspectAssets(const std::string& folder, bool cleanup) {
  AssetUsage usage;
  std::error_code ec;
  if (!std::filesystem::is_directory(
          std::filesystem::symlink_status(folder, ec))) {
    return usage;
  }
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (!std::filesystem::is_regular_file(entry.symlink_status(ec))) {
      usage.valid = false;
      break;
    }
    if (entry.path().extension() == ".json") continue;
    if (cleanup && std::filesystem::file_time_type::clock::now() -
                           entry.last_write_time(ec) >
                       std::chrono::hours(24)) {
      auto metadata = entry.path();
      metadata.replace_extension(".json");
      std::string bytes, error;
      json record;
      if (ReadRegularFile(metadata.string(), 1024, bytes, error)) {
        record = json::parse(bytes, nullptr, false);
      }
      if (!JsonValue(record, "committed", false)) {
        std::filesystem::remove(entry.path(), ec);
        if (!ec) {
          std::filesystem::remove(metadata, ec);
          continue;
        }
      }
    }
    uintmax_t bytes = entry.file_size(ec);
    if (ec || bytes > kGlobalAssetBytes ||
        usage.bytes > kGlobalAssetBytes - bytes) {
      usage.valid = false;
      break;
    }
    usage.bytes += static_cast<size_t>(bytes);
    if (++usage.count > 64) {
      usage.valid = false;
      break;
    }
  }
  return usage;
}
}  // namespace

HostSession::~HostSession() {
  stop.Wake();
  if (reader.joinable()) reader.join();
}

bool HostSession::Send(json frame) {
  std::lock_guard lock(send_mutex);
  frame["v"] = kProtocol;
  frame["session_id"] = id;
  frame["generation"] = generation;
  return socket && WriteFrame(socket.Get(), frame);
}

SessionHost::SessionHost(std::string epoch, size_t byte_limit,
                         std::string executable, std::string directory,
                         size_t event_limit)
    : epoch_(std::move(epoch)),
      executable_(std::move(executable)),
      directory_(std::move(directory)),
      byte_limit_(byte_limit),
      event_limit_(event_limit) {
  schedule_state_ = ReadSchedules();
  scheduled_view_ = ScheduleControl({{"action", "list"}});
}

HostReplay SessionHost::Publish(const std::string& session,
                                const std::string& generation, json value) {
  std::lock_guard lock(mutex_);
  return PublishLocked(session, generation, std::move(value));
}

HostReplay SessionHost::PublishLocked(const std::string& session,
                                      const std::string& generation,
                                      json value) {
  const std::string type = JsonValue(value, "type", "");
  const std::string kind = JsonValue(value, "kind", "");
  HostNotice notice;
  notice.session = session;
  if (type == "turn.completed" || type == "approval.requested" ||
      type == "error" || kind == "error") {
    notice.attention_id = epoch_ + ":" + std::to_string(sequence_ + 1);
    value["attention_id"] = notice.attention_id;
  }
  auto owner = sessions_.find(session);
  notice.wake = kind == "scheduled.changed" || kind == "management.changed" ||
                kind == "metadata" || kind == "activated" ||
                kind == "deactivated" ||
                (owner != sessions_.end() && !owner->second->run_id.empty());
  if (kind == "deleted") notices_.erase(session);
  value["epoch"] = epoch_;
  value["sequence"] = ++sequence_;
  value["session_id"] = session;
  value["generation"] = generation;
  value["v"] = kProtocol;
  HostReplay published{sequence_, JsonDump(value)};
  replay_bytes_ += published.frame.size();
  replay_.push_back(published);
  while (replay_bytes_ > byte_limit_ || replay_.size() > event_limit_) {
    replay_bytes_ -= replay_.front().frame.size();
    replay_.pop_front();
  }
  if (!notice.attention_id.empty() || notice.wake) {
    auto& pending = notices_[notice.session];
    pending.session = notice.session;
    pending.wake |= notice.wake;
    if (!notice.attention_id.empty()) {
      pending.attention_id = std::move(notice.attention_id);
    }
  }
  changed_.notify_all();
  return published;
}

uint64_t SessionHost::Cursor() const {
  std::lock_guard lock(mutex_);
  return sequence_;
}

ReplayBatch SessionHost::ReadReplay(uint64_t next, bool valid,
                                    uint64_t watermark) const {
  std::lock_guard lock(mutex_);
  ReplayBatch batch;
  batch.cursor = sequence_;
  batch.reset = !valid || next > sequence_ ||
                (!replay_.empty() && next + 1 < replay_.front().sequence);
  if (batch.reset) return batch;
  for (const HostReplay& event : replay_) {
    if (event.sequence > next && event.sequence <= watermark) {
      batch.events.push_back(event);
    }
  }
  return batch;
}

void SessionHost::WaitForReplay(uint64_t cursor, std::chrono::seconds timeout) {
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, timeout,
                    [&] { return stopping_ || sequence_ > cursor; });
}

std::vector<HostNotice> SessionHost::WaitForNotices() {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] { return stopping_ || !notices_.empty(); });
  std::vector<HostNotice> notices;
  notices.reserve(notices_.size());
  for (auto& [session, notice] : notices_) {
    notices.push_back(std::move(notice));
  }
  notices_.clear();
  return notices;
}

void SessionHost::Stop() {
  stopping_ = true;
  changed_.notify_all();
}

std::vector<SessionInfo> SessionHost::DiscoverSessions() const {
  return ListSessions(SessionScope::kAll);
}

void SessionHost::LoadDrafts() {
  std::error_code ec;
  const std::string folder = directory_ + "/drafts";
  if (!EnsurePrivateDirectory(folder)) return;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (sessions_.size() >= 4096) break;
    if (entry.path().extension() != ".json") continue;
    std::string bytes, error;
    if (!ReadRegularFile(entry.path().string(), 4096, bytes, error)) continue;
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
  PublishLocked(id, session.generation,
                {{"kind", "metadata"}, {"metadata", std::move(after)}});
  return true;
}

bool SessionHost::RefreshCatalogue(bool force) {
  std::lock_guard scan(scan_mutex_);
  if (!force &&
      std::chrono::steady_clock::now() - scanned_ < std::chrono::seconds(1)) {
    return false;
  }
  scanned_ = std::chrono::steady_clock::now();
  const auto list = DiscoverSessions();
  std::lock_guard lock(mutex_);
  bool changed = false;
  for (const SessionInfo& item : list) {
    const std::string id = HashHex(item.path);
    auto [it, inserted] = sessions_.try_emplace(id, nullptr);
    if (inserted) {
      it->second = std::make_shared<HostSession>();
      it->second->id = id;
      it->second->path = item.path;
    }
    auto& session = *it->second;
    if (session.closing || session.status == "updating" ||
        session.status == "deleting") {
      continue;
    }
    session.cwd = item.cwd;
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
  return changed;
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
    std::string error;
    ActivateLocked(session, error, lock, false);
  }
}

std::shared_ptr<HostSession> SessionHost::CreateSession(
    const std::string& cwd, const std::string& path, const std::string& title,
    std::string& error) {
  if (sessions_.size() >= 4096) {
    error = "session catalogue limit reached";
    return {};
  }
  if (!EnsurePrivateDirectory(
          std::filesystem::path(path).parent_path().string()) ||
      !EnsurePrivateDirectory(directory_ + "/drafts")) {
    error = "cannot create private session directory";
    return {};
  }
  auto session = std::make_shared<HostSession>();
  session->cwd = cwd;
  session->path = path;
  session->id = HashHex(path);
  session->title = title.empty() ? "New conversation" : title;
  session->draft_title = title;
  session->status = "draft";
  session->updated = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  auto written =
      ToolWritePrivateFile(directory_ + "/drafts/" + session->id + ".json",
                           JsonDump({{"cwd", cwd},
                                     {"path", path},
                                     {"id", session->id},
                                     {"title", title},
                                     {"updated", session->updated}}));
  if (!written.Ok()) {
    error = "cannot persist draft identity";
    return {};
  }
  sessions_[session->id] = session;
  return session;
}

Connection SessionHost::OpenRuntime(const HostSession& session, bool create,
                                    std::string& error) const {
  Options options;
  if (!session.launch.empty()) {
    const std::string model = JsonValue(session.launch, "model", "");
    if (!model.empty()) options.overrides["UAGENT_MODEL"] = model;
    options.overrides["UAGENT_APPROVAL"] =
        JsonValue(session.launch, "permissions", "prompt");
  }
  Connection connection =
      create ? Open(executable_, session.cwd, session.path,
                    session.draft_title, options, error)
               : Connect(session.path);
  // Record which executable this worker runs before anyone can adopt it:
  // a later host compares this against the binary on disk. Written only
  // for fresh spawns; a dead-on-arrival spawn leaves no socket, so the
  // next attempt overwrites the record with its own spawn.
  if (create && connection.socket)
    WriteWorkerBinary(session.path, ExecutableIdentity(executable_));
  return connection;
}

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

void SessionHost::DeactivateLocked(HostSession& session) {
  session.closing = false;
  session.pid = -1;
  {
    std::lock_guard send_lock(session.send_mutex);
    session.generation.clear();
  }
  session.status = "saved";
  session.error.clear();
  session.state["activity"] = "Ready";
  PublishLocked(session.id, "",
                {{"kind", "deactivated"}, {"metadata", Metadata(session)}});
}

void SessionHost::Received(HostSession* session, json frame) {
  std::unique_lock lock(mutex_);
  auto owner = sessions_.find(session->id);
  if (owner == sessions_.end() || owner->second.get() != session ||
      JsonValue(frame, "session_id", "") != session->id ||
      JsonValue(frame, "generation", "") != session->generation) {
    return;
  }
  const uint64_t sequence = JsonValue(frame, "sequence", uint64_t{0});
  if (sequence) {
    if (sequence <= session->runtime_sequence) {
      return;
    }
    session->runtime_sequence = sequence;
  }
  const std::string kind = JsonValue(frame, "kind", "");
  if (kind == "outcome") {
    if (!ResolveOutcome(*session, frame)) {
      if (!session->run_id.empty() && !JsonValue(frame, "accepted", false)) {
        session->error =
            JsonValue(frame, "error", "scheduled submission failed");
        session->run_result = "failed";
      }
      return;
    }
    if (JsonValue(JsonValue(frame, "result", json::object()), "forked",
                  false)) {
      lock.unlock();
      RefreshCatalogue(true);
      lock.lock();
    }
  } else {
    ApplyRuntimeFrame(*session, frame);
  }
  PublishLocked(session->id, session->generation, std::move(frame));
  if (kind == "gap") {
    lock.unlock();
    session->Send({{"kind", "refresh"}, {"request_id", RandomToken(16)}});
  }
  changed_.notify_all();
}

bool SessionHost::RecycleStaleWorkerLocked(
    const std::shared_ptr<HostSession>& session,
    std::unique_lock<std::mutex>& lock) {
  if (!WorkerBinaryStale(ExecutableIdentity(executable_),
                         ReadWorkerBinary(session->path)))
    return false;
  // Binary upgraded since this worker spawned: graceful close, then the
  // caller spawns fresh. Same semantics as user-initiated close of a busy
  // session; a worker that ignores close keeps serving (fail open, retried
  // on the next touch).
  DebugLog("worker_binary_recycle", {{"session", session->id}});
  session->Send({{"kind", "close"}, {"request_id", RandomToken(16)}});
  changed_.wait_for(lock, std::chrono::seconds(5),
                    [&] { return session->exited.load(); });
  if (!session->exited) return false;
  session->pid = -1;
  if (session->reader.joinable()) {
    lock.unlock();
    session->reader.join();
    lock.lock();
  }
  return true;
}

bool SessionHost::ActivateLocked(const std::shared_ptr<HostSession>& session,
                                 std::string& error,
                                 std::unique_lock<std::mutex>& lock,
                                 bool create) {
  if (session->closing) {
    error = "session is closing";
    return false;
  }
  bool create_now = create;
  for (int attempt = 0;; ++attempt) {
    if (session->pid > 0 && !session->exited) {
      if (!RecycleStaleWorkerLocked(session, lock)) return true;
      create_now = true;  // recycled: fall through to a fresh spawn
    }
  if (session->connecting) {
    error = "session is starting";
    return false;
  }
  session->connecting = true;
  if (session->reader.joinable()) {
    lock.unlock();
    session->reader.join();
    lock.lock();
    auto owner = sessions_.find(session->id);
    if (stopping_ || owner == sessions_.end() || owner->second != session) {
      session->connecting = false;
      error = "session was closed while joining its prior runtime";
      return false;
    }
  }
  lock.unlock();
  auto connected = OpenRuntime(*session, create_now, error);
  lock.lock();
  session->connecting = false;
  auto owner = sessions_.find(session->id);
  if (stopping_ || owner == sessions_.end() || owner->second != session) {
    error = "session was closed while connecting";
    return false;
  }
  if (!connected.socket || !session->stop.Open()) return false;
  if (session->generation != connected.generation) {
    session->runtime_sequence = 0;
  }
  {
    std::lock_guard send_lock(session->send_mutex);
    session->socket = std::move(connected.socket);
    session->generation = connected.generation;
  }
  session->pid = connected.pid;
  session->exited = false;
  session->status = "starting";
  session->error.clear();
  session->reader = std::thread([this, session = session.get()] {
    ReadFrames(session->socket.Get(), session->stop.read.Get(), kFrameBytes,
               [&](json frame) {
                 Received(session, std::move(frame));
                 return !stopping_;
               });
    std::lock_guard state_lock(mutex_);
    auto current = sessions_.find(session->id);
    if (current != sessions_.end() && current->second.get() == session) {
      FailPending(*session);
      session->turn_active = false;
      session->pending = nullptr;
      if (session->closing) {
        DeactivateLocked(*session);
      } else {
        session->status = "interrupted";
        session->state["activity"] = "Interrupted";
        PublishLocked(session->id, session->generation, {{"kind", "closed"}});
      }
    }
    session->exited = true;
    changed_.notify_all();
  });
  // Adopted a live worker through Connect: it may predate the executable
  // (host restarted over it). Recycle through the same gate, then spawn.
  // Fresh spawns match the record OpenRuntime just wrote and skip this.
  if (RecycleStaleWorkerLocked(session, lock)) {
    if (attempt > 0) {
      error = "worker binary changed during activation";
      return false;
    }
    create_now = true;
    continue;
  }
  PublishLocked(session->id, session->generation,
                {{"kind", "activated"}, {"metadata", Metadata(*session)}});
  return true;
  }  // for (attempt): single pass unless a stale worker recycled above
}

void SessionHost::ApplyRuntimeFrame(HostSession& session, json& frame) {
  const std::string kind = JsonValue(frame, "kind", "");
  if (kind == "state") {
    json next = JsonValue(frame, "state", json::object());
    json view = JsonValue(session.state, "view", json::object());
    const json checkpoint_view = JsonValue(next, "view", json::object());
    if (checkpoint_view.is_object()) {
      for (auto it = checkpoint_view.begin(); it != checkpoint_view.end();
           ++it) {
        if (it.key() != "blocks") view[it.key()] = it.value();
      }
    }
    if (const json* blocks = JsonArray(checkpoint_view, "blocks")) {
      for (const json& block : *blocks) MergeDisplayBlock(view, block);
    }
    if (!view.contains("blocks")) view["blocks"] = json::array();
    next["view"] = std::move(view);
    session.state = std::move(next);
    session.pending = JsonValue(frame, "pending", json(nullptr));
    session.turn_active = JsonValue(frame, "busy", false);
    session.guidance = JsonValue(frame, "guidance", uint64_t{0});
    session.status = session.closing                           ? "closing"
                     : !session.pending.is_null()              ? "waiting"
                     : !session.state.contains("route")        ? "starting"
                     : session.turn_active                     ? "running"
                     : JsonValue(frame, "command_busy", false) ? "processing"
                                                               : "idle";
    if (JsonValue(frame, "checkpoint", false)) {
      if (!session.run_id.empty() && !session.turn_active) {
        if (const auto* blocks = JsonArray(session.state["view"], "blocks")) {
          for (auto it = blocks->rbegin(); it != blocks->rend(); ++it) {
            if (JsonValue(*it, "kind", "") != "turn_summary") continue;
            const std::string outcome =
                JsonValue((*it)["summary"], "outcome", "error");
            session.run_result = outcome == "complete"      ? "completed"
                                 : outcome == "interrupted" ? "interrupted"
                                                            : "failed";
            break;
          }
        }
      }
      if (!session.run_result.empty()) session.run_checkpoint = true;
      session.live_truncated = false;
      session.active_exchanges.clear();
      session.updated = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
      const std::string title = JsonValue(session.state, "title", "");
      if (!title.empty()) session.title = title;
    }
    frame["updated"] = session.updated;
    return;
  }
  if (kind == "activity") {
    session.state["activity"] = JsonValue(frame, "activity", "Ready");
    session.turn_active = JsonValue(frame, "busy", false);
    return;
  }
  if (kind == "gap") {
    session.live_truncated = true;
    return;
  }
  if (kind == "error") {
    session.error = JsonValue(frame, "error", "worker failed");
    session.status = "failed";
    return;
  }
  if (kind != "event") return;
  const std::string type = JsonValue(frame, "type", "");
  if (!session.run_id.empty()) {
    if (type == "turn.completed") {
      const std::string outcome = JsonValue(frame["data"], "outcome", "error");
      session.run_result = outcome == "complete"      ? "completed"
                           : outcome == "interrupted" ? "interrupted"
                                                      : "failed";
      if (session.run_result == "failed") {
        session.error = "Turn ended: " + outcome;
      }
    } else if (type == "turn.stopped") {
      session.run_result = "interrupted";
    } else if (type == "error") {
      session.error = JsonValue(frame["data"], "error", "scheduled run failed");
    }
  }
  ApplySessionEvent(session.state, type, frame["data"]);
  if (type == "tool.result") {
    const json data = JsonValue(frame, "data", json::object());
    const std::string detail = JsonValue(data, "detail_id", "");
    if (!detail.empty()) {
      if (session.active_exchanges.size() >= 32) {
        session.active_exchanges.erase(session.active_exchanges.begin());
      }
      session.active_exchanges[detail] = {
          {"request",
           {{"name", JsonValue(data, "name", "")},
            {"arguments", "retained with tool call"}}},
          {"response", JsonValue(data, "result", "")},
          {"complete", true}};
    }
  }
  if (type == "message.changed") {
    const json block = frame["data"]["block"];
    session.incoming =
        std::max(session.incoming, JsonValue(block, "incoming", uint64_t{0}));
  }
}

std::chrono::steady_clock::time_point SessionHost::NextScheduleDeadline()
    const {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(24);
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  if (const auto* tasks = JsonArray(schedule_state_, "tasks")) {
    for (const json& task : *tasks) {
      const int64_t next = JsonValue(task, "next", int64_t{0});
      if (!JsonValue(task, "enabled", false) || next <= 0) continue;
      deadline = std::min(
          deadline, std::chrono::steady_clock::now() +
                        std::chrono::seconds(std::max(int64_t{0}, next - now)));
    }
  }
  return deadline;
}

void SessionHost::RecordRun(const RunUpdate& update) {
  if (UpdateScheduledRun(update.id, update.status, update.error)
          .contains("error")) {
    run_updates_.push_back(update);
  }
}

std::shared_ptr<HostSession> SessionHost::StartScheduledRun(const json& run) {
  const json& definition = run["definition"];
  const std::string cwd = JsonValue(run, "cwd", "");
  const std::string id = JsonValue(run, "id", "");
  if (JsonValue(definition, "environment", "local") == "worktree") {
    auto created =
        CaptureProcess({"git", "-C", JsonValue(definition, "cwd", ""),
                        "worktree", "add", "--detach", cwd, "HEAD"},
                       30);
    if (!created.Ok()) {
      RecordRun({id, "failed",
                 "Cannot create worktree: " +
                     Utf8Prefix(created.output + created.error, 1024)});
      return {};
    }
  }
  std::string error;
  auto session = CreateSession(cwd, JsonValue(run, "session_path", ""),
                               JsonValue(run, "title", "Scheduled run"), error);
  if (!session) {
    RecordRun({id, "failed", error});
    return {};
  }
  session->run_id = id;
  session->task_id = JsonValue(run, "task_id", "");
  session->launch = definition;
  session->launch_prompt = JsonValue(definition, "prompt", "");
  return session;
}

bool SessionHost::RecoverSchedules(
    std::vector<std::shared_ptr<HostSession>>& activate) {
  const json stored = ReadSchedules();
  const json* runs = JsonArray(stored, "runs");
  if (!runs) return false;
  for (const json& run : *runs) {
    const std::string status = JsonValue(run, "status", "");
    if (!ScheduledRunActive(status) || status == "queued") continue;
    const std::string id = JsonValue(run, "id", "");
    auto found = sessions_.find(JsonValue(run, "session_id", ""));
    if (found == sessions_.end()) {
      RecordRun(
          {id, "interrupted",
           "Session runtime unavailable. Inspect this run before retrying."});
      continue;
    }
    found->second->run_id = id;
    found->second->task_id = JsonValue(run, "task_id", "");
    activate.push_back(found->second);
  }
  return true;
}

ScheduleTick SessionHost::TickSchedules() {
  ScheduleTick tick;
  auto pending_updates = std::exchange(run_updates_, {});
  for (const auto& update : pending_updates) RecordRun(update);
  const json* runs = JsonArray(schedule_state_, "runs");
  if (!runs) return tick;
  size_t workers = 0;
  std::vector<RunUpdate> updates;
  for (auto& [session_id, session] : sessions_) {
    if (session->run_id.empty()) continue;
    if (session->pid > 0 && !session->exited) ++workers;
    auto run = std::find_if(runs->begin(), runs->end(), [&](const json& item) {
      return JsonValue(item, "id", "") == session->run_id;
    });
    if (run == runs->end()) continue;
    const std::string prior = JsonValue(*run, "status", "");
    if (!ScheduledRunActive(prior)) continue;
    if (prior == "stopping" && !session->stop_sent) {
      session->stop_sent = true;
      session->launch_prompt.clear();
      tick.commands.push_back(
          {session, {{"kind", "interrupt"}, {"request_id", RandomToken(16)}}});
      session->run_result = "interrupted";
    }
    if (!session->launch_prompt.empty() && session->state.contains("route") &&
        !session->turn_active && session->pending.is_null()) {
      auto prompt = std::exchange(session->launch_prompt, "");
      tick.commands.push_back({session,
                               {{"kind", "submit"},
                                {"request_id", session->run_id},
                                {"client_request_id", session->run_id},
                                {"text", prompt}}});
      updates.push_back({session->run_id, "running", ""});
    }
    if (!session->pending.is_null()) {
      updates.push_back({session->run_id, "waiting", ""});
    } else if (prior == "waiting") {
      updates.push_back({session->run_id, "running", ""});
    }
    bool background = false;
    if (const json* activities = JsonArray(session->state, "activities")) {
      for (const json& activity : *activities) {
        const std::string state =
            JsonValue(activity, "status", JsonValue(activity, "state", ""));
        if (state == "running" || state == "starting" || state == "stopping" ||
            state == "finishing") {
          background = true;
        }
      }
    }
    if (session->exited || !session->error.empty() ||
        (!session->run_result.empty() &&
         (session->run_checkpoint ||
          (session->stop_sent && !session->turn_active)) &&
         !session->turn_active && !background)) {
      const std::string status = !session->error.empty() ? "failed"
                                 : session->run_result.empty()
                                     ? "interrupted"
                                     : session->run_result;
      if (UpdateScheduledRun(session->run_id, status, session->error)
              .contains("error")) {
        continue;
      }
      session->closing = true;
      session->status = "closing";
      tick.commands.push_back(
          {session, {{"kind", "close"}, {"request_id", RandomToken(16)}}});
    }
  }
  for (const auto& update : updates) RecordRun(update);

  constexpr size_t kScheduledConcurrency = 2;
  const size_t slots =
      workers < kScheduledConcurrency ? kScheduledConcurrency - workers : 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  bool due = std::any_of(runs->begin(), runs->end(), [](const json& run) {
    return JsonValue(run, "status", "") == "queued";
  });
  if (const json* tasks = JsonArray(schedule_state_, "tasks")) {
    for (const json& task : *tasks) {
      due |= JsonValue(task, "enabled", false) &&
             JsonValue(task, "next", int64_t{0}) > 0 &&
             JsonValue(task, "next", int64_t{0}) <= now;
    }
  }
  if (due) {
    const json claimed = ClaimScheduledRuns(now, slots);
    if (const json* claimed_runs = JsonArray(claimed, "runs")) {
      for (const json& run : *claimed_runs) {
        if (auto session = StartScheduledRun(run)) {
          tick.activate.push_back(std::move(session));
        }
      }
    }
  }
  return tick;
}

std::vector<json> SessionHost::RefreshInvalidations(
    const std::vector<std::string>& projects) {
  std::vector<json> events;
  std::map<std::string, FileStamp> prompts;
  prompts[(std::filesystem::path(GlobalBase()) / "system-prompt.json")
              .string()] = {};
  for (const std::string& project : projects) {
    prompts[(ProjectBase(project) / "system-prompt.json").string()] = {};
  }
  for (auto& [path, stamp] : prompts) stamp = SnapshotFile(path);
  if (prompts != prompt_stamps_) {
    prompt_stamps_ = std::move(prompts);
    events.push_back({{"kind", "management.changed"}});
  }
  const FileStamp library = SnapshotFile(LibraryChangePath());
  if (library != library_stamp_) {
    library_stamp_ = library;
    events.push_back({{"kind", "management.changed"}});
  }
  const FileStamp schedule = SnapshotFile(SchedulePath());
  if (schedule != schedule_stamp_) {
    schedule_stamp_ = schedule;
    schedule_state_ = ReadSchedules();
    scheduled_view_ = ScheduleControl({{"action", "list"}});
    events.push_back(
        {{"kind", "scheduled.changed"}, {"scheduled", scheduled_view_}});
  }
  return events;
}

std::vector<std::string> SessionHost::InvalidationPaths(
    const std::vector<std::string>& projects) const {
  std::vector<std::string> paths{
      SchedulePath(), LibraryChangePath(),
      (std::filesystem::path(GlobalBase()) / "system-prompt.json").string()};
  for (const std::string& project : projects) {
    paths.push_back((ProjectBase(project) / "system-prompt.json").string());
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::vector<std::string> SessionHost::PresencePaths() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> paths{UagentDir(kHistoryDir)};
  for (const auto& [id, session] : sessions_) {
    paths.push_back(
        std::filesystem::path(session->path).parent_path().string());
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

HostWaitState SessionHost::RunSchedules(bool& recovered) {
  HostWaitState wait;
  ScheduleTick tick;
  std::vector<std::string> projects;
  {
    std::unique_lock lock(mutex_);
    std::vector<std::shared_ptr<HostSession>> recovery;
    if (!recovered) recovered = RecoverSchedules(recovery);
    if (recovered) tick = TickSchedules();
    auto activate = [&](const std::shared_ptr<HostSession>& session,
                        bool create, const std::string& failed) {
      std::string error;
      if (!ActivateLocked(session, error, lock, create)) {
        UpdateScheduledRun(session->run_id, failed,
                           error.empty() ? "Session runtime unavailable. "
                                           "Inspect this run before retrying."
                                         : error);
      } else if (PublishMetadata(session->id, *session)) {
        changed_.notify_all();
        wait.wake = true;
      }
    };
    for (const auto& session : recovery) {
      activate(session, false, "interrupted");
    }
    for (const auto& session : tick.activate) activate(session, true, "failed");
    wait.deadline = NextScheduleDeadline();
    for (const auto& [id, session] : sessions_) {
      projects.push_back(session->cwd);
    }
  }
  for (auto& item : tick.commands) {
    if (!item.session->Send(std::move(item.command))) {
      std::lock_guard lock(mutex_);
      item.session->error = "cannot deliver scheduled command";
      item.session->run_result = "failed";
    }
  }
  {
    std::lock_guard lock(mutex_);
    for (json event : RefreshInvalidations(projects)) {
      PublishLocked("", "", std::move(event));
    }
  }
  wait.paths = InvalidationPaths(projects);
  return wait;
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
  return {{"id", session.id},
          {"task_id", session.task_id},
          {"run_id", session.run_id},
          {"cwd", session.cwd},
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
          {"pending", !session.pending.is_null()},
          {"updated", session.updated}};
}

json SessionHost::LiveSnapshot(const HostSession& session) const {
  return {{"v", kProtocol},
          {"epoch", epoch_},
          {"cursor", sequence_},
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
  return {{"cursor", sequence_},
          {"sessions", std::move(sessions)},
          {"scheduled", scheduled_view_}};
}

std::string SessionHost::AssetPath(const std::string& id) const {
  std::lock_guard lock(mutex_);
  auto found = sessions_.find(id);
  return found == sessions_.end() ? std::string{} : found->second->path;
}

AssetStoreResult SessionHost::StoreAsset(const std::string& session_id,
                                         const std::string& bytes,
                                         std::string name) {
  std::string session_path;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(session_id);
    if (found == sessions_.end()) {
      return {{}, "unknown session", 404};
    }
    session_path = found->second->path;
  }
  std::lock_guard assets(asset_mutex_);
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(session_id);
    if (found == sessions_.end() || found->second->path != session_path ||
        found->second->status == "deleting") {
      return {{}, "session changed while storing attachment", 409};
    }
  }
  const std::string folder = session_path + ".assets";
  if (!EnsurePrivateDirectory(folder)) {
    return {{}, "cannot create asset storage", 500};
  }
  if (std::chrono::steady_clock::now() - assets_scanned_ >=
      std::chrono::minutes(1)) {
    assets_scanned_ = std::chrono::steady_clock::now();
    asset_bytes_ = 0;
    std::error_code ec;
    std::vector<std::filesystem::path> folders{UagentDir(kHistoryDir)};
    for (const auto& entry :
         std::filesystem::directory_iterator(folders.front(), ec)) {
      if (std::filesystem::is_directory(entry.symlink_status(ec)) &&
          !entry.path().string().ends_with(".assets")) {
        folders.push_back(entry.path());
      }
      if (folders.size() > 4096) {
        asset_bytes_ = kGlobalAssetBytes;
        break;
      }
    }
    size_t scanned = 0;
    for (const auto& directory : folders) {
      for (const auto& entry :
           std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.path().string().ends_with(".json.assets") ||
            !std::filesystem::is_directory(entry.symlink_status(ec))) {
          continue;
        }
        AssetUsage usage = InspectAssets(entry.path().string(), true);
        if (++scanned > 4096 || !usage.valid ||
            usage.bytes > kGlobalAssetBytes - asset_bytes_) {
          asset_bytes_ = kGlobalAssetBytes;
          break;
        }
        asset_bytes_ += usage.bytes;
      }
      if (asset_bytes_ >= kGlobalAssetBytes) break;
    }
  }
  AssetUsage usage = InspectAssets(folder, true);
  if (!usage.valid || usage.count >= 64 ||
      bytes.size() >
          kSessionAssetBytes - std::min(usage.bytes, kSessionAssetBytes) ||
      bytes.size() >
          kGlobalAssetBytes - std::min(asset_bytes_, kGlobalAssetBytes)) {
    return {{}, "attachment storage limit reached", 413};
  }
  std::string mime = RasterMime(bytes);
  const bool image = !mime.empty();
  name = Utf8Prefix(std::filesystem::path(name).filename().string(), 240);
  if (name.empty()) {
    name = image ? "image" + ImageExtension(mime) : "attachment";
  }
  for (char& ch : name) {
    if (static_cast<unsigned char>(ch) < 32) ch = '_';
  }
  if (mime.empty()) {
    mime = AttachmentMime(name);
    if (mime.starts_with("image/")) mime = "application/octet-stream";
  }
  std::string id = RandomToken(16), stem = folder + "/" + id;
  if (id.empty() || !ToolWritePrivateFile(stem + ".data", bytes).Ok()) {
    return {{}, "cannot persist upload", 500};
  }
  asset_bytes_ += bytes.size();
  if (!ToolWritePrivateFile(stem + ".json", JsonDump({{"mime", mime},
                                                      {"name", name},
                                                      {"image", image},
                                                      {"extension", ".data"},
                                                      {"bytes", bytes.size()},
                                                      {"committed", false}}))
           .Ok()) {
    return {{}, "cannot persist upload metadata", 500};
  }
  return {{{"id", id},
           {"name", name},
           {"mime", mime},
           {"image", image},
           {"bytes", bytes.size()}},
          {},
          200};
}

SnapshotResult SessionHost::Snapshot(const std::string& id,
                                     const SnapshotQuery& query) {
  std::shared_ptr<HostSession> session;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(id);
    if (found == sessions_.end()) return {{{"error", "unknown session"}}, 404};
    session = found->second;
    if (session->pid > 0 && !query.has_before && !query.has_detail &&
        !query.has_http) {
      return {LiveSnapshot(*session), 200};
    }
  }
  if (query.has_http) {
    json exchange;
    {
      std::lock_guard lock(mutex_);
      exchange = FindHttpExchange(session->state, query.http);
    }
    if (exchange.is_null()) {
      std::lock_guard reading(history_mutex_);
      auto loaded = SessionStore::Inspect(session->path);
      if (loaded.record) {
        const json& display = loaded.record->state.display;
        exchange =
            query.http == "latest"
                ? FindHttpExchange(
                      JsonValue(JsonValue(display, "facts", json::object()),
                                "http-latest", json::object()),
                      query.http)
                : FindHttpExchange(display, query.http);
      }
    }
    if (exchange.is_null()) {
      return {{{"error", "HTTP exchange was not captured"}}, 404};
    }
    if (query.part != "request" && query.part != "response") {
      return {{{"error", "choose request or response"}}, 400};
    }
    int64_t offset = 0;
    ParseInt64(query.offset.c_str(), offset);
    json body = ReadPrivateArtifact(
        JsonValue(exchange, (query.part + "_path").c_str(), ""),
        static_cast<size_t>(std::max(int64_t{0}, offset)));
    body["exchange"] = exchange;
    const int status = body.contains("error") ? 404 : 200;
    return {std::move(body), status};
  }
  if (query.raw && query.has_detail) {
    std::string path;
    json inline_exchange = nullptr;
    bool turn_active = false;
    {
      std::lock_guard lock(mutex_);
      turn_active = session->turn_active;
      auto active = session->active_exchanges.find(query.detail);
      if (active != session->active_exchanges.end()) {
        inline_exchange = active->second;
        turn_active = true;
      }
      const json* view = JsonObject(session->state, "view");
      const json* blocks = view ? JsonArray(*view, "blocks") : nullptr;
      if (blocks) {
        for (const json& block : *blocks) {
          if (JsonValue(block, "detail_id", "") == query.detail) {
            path = JsonValue(block, "exchange_path", "");
            break;
          }
          if (const json* tools = JsonArray(block, "tools")) {
            auto found = std::find_if(
                tools->begin(), tools->end(), [&](const json& tool) {
                  return JsonValue(tool, "detail_id", "") == query.detail;
                });
            if (found != tools->end()) {
              path = JsonValue(*found, "exchange_path", "");
              const json retained_request = {
                  {"name", JsonValue(*found, "name", "")},
                  {"arguments", JsonValue(*found, "arguments", "")}};
              if (!inline_exchange.is_null()) {
                inline_exchange["request"] = retained_request;
              } else if (path.empty()) {
                inline_exchange = {{"request", retained_request},
                                   {"response", ""},
                                   {"complete", false}};
              }
              if (!path.empty()) break;
            }
          }
        }
      }
    }
    int64_t offset = 0;
    ParseInt64(query.offset.c_str(), offset);
    const size_t start = static_cast<size_t>(std::max(int64_t{0}, offset));
    if (!path.empty()) {
      json body = ReadPrivateArtifact(path, start);
      const int status = body.contains("error") ? 404 : 200;
      return {std::move(body), status};
    }
    if (turn_active && !inline_exchange.is_null()) {
      std::string text = JsonDump(inline_exchange);
      return {{{"text", text.substr(std::min(text.size(), start))},
               {"next", text.size()},
               {"bytes", text.size()},
               {"more", false}},
              200};
    }
  }
  std::lock_guard reading(history_mutex_);
  auto loaded = SessionStore::Inspect(session->path);
  json state = json::object();
  if (loaded.record) {
    auto& record = *loaded.record;
    Conversation conversation;
    if (!conversation.Restore(std::move(record.state.messages),
                              std::move(record.state.message_kinds),
                              std::move(record.state.archive),
                              record.state.archive_dropped_segments,
                              std::move(record.state.tool_displays),
                              record.state.display)) {
      return {{{"error", "invalid conversation metadata"}}, 422};
    }
    int64_t position = 0;
    if (query.has_detail) {
      ParseInt64(query.offset.c_str(), position);
      size_t offset = static_cast<size_t>(std::max(int64_t{0}, position));
      json detail =
          query.raw ? ConversationExchange(conversation, query.detail, offset)
          : query.artifact
              ? ReadPrivateArtifact(
                    JsonValue(JsonValue(conversation.DisplayFacts(),
                                        query.detail.c_str(), json::object()),
                              "artifact", ""),
                    offset)
              : ConversationDetail(conversation, query.detail, offset);
      const int status = detail.contains("error") ? 404 : 200;
      return {std::move(detail), status};
    }
    ParseInt64(query.before.c_str(), position);
    state = {
        {"view", ConversationView(conversation, static_cast<uint64_t>(std::max(
                                                    int64_t{0}, position)))},
        {"usage", UsageJson(record.state.usage)},
        {"context_tokens", record.state.context_tokens},
        {"model", record.metadata.model},
        {"turns", record.metadata.turns},
        {"statistics", conversation.Statistics()},
        {"http", JsonValue(JsonValue(conversation.DisplayFacts(), "http-latest",
                                     json::object()),
                           "http", json::array())}};
  } else if (PathExists(session->path)) {
    return {{{"error", loaded.status.message}}, 422};
  }
  std::lock_guard lock(mutex_);
  auto owner = sessions_.find(session->id);
  if (owner != sessions_.end()) session = owner->second;
  if (session->pid > 0 && !query.has_before) {
    return {LiveSnapshot(*session), 200};
  }
  return {{{"v", kProtocol},
           {"epoch", epoch_},
           {"cursor", sequence_},
           {"metadata", Metadata(*session)},
           {"state", std::move(state)},
           {"pending", session->pending},
           {"live", json::array()}},
          200};
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
      const int64_t updated =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
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
        if (!display.empty()) {
          if (display.size() > 128 || display.find('/') != std::string::npos ||
              display.find('\\') != std::string::npos ||
              display.find_first_of("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\x7f") !=
                  std::string::npos) {
            result.error = "invalid attachment name";
            break;
          }
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
          for (size_t i = 0; i < committed; ++i) {
            claims[i].second["committed"] = false;
            ToolWritePrivateFile(claims[i].first, JsonDump(claims[i].second));
          }
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
          for (auto& [path, asset] : claims) {
            asset["committed"] = false;
            ToolWritePrivateFile(path, JsonDump(asset));
          }
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
