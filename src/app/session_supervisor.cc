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
std::string SessionHost::RunResultFor(const std::string& outcome) {
  return outcome == "complete"      ? "completed"
         : outcome == "interrupted" ? "interrupted"
                                    : "failed";
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
  session->updated = NowMillis();
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
  Connection connection = create ? Open(executable_, session.cwd, session.path,
                                        session.draft_title, options, error)
                                 : Connect(session.path);
  // Record which executable this worker runs before anyone can adopt it:
  // a later host compares this against the binary on disk. Written only
  // for fresh spawns; a dead-on-arrival spawn leaves no socket, so the
  // next attempt overwrites the record with its own spawn.
  if (create && connection.socket) {
    WriteWorkerBinary(session.path, ExecutableIdentity(executable_));
  }
  return connection;
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
  replay_.Publish(epoch_, session.id, "",
                  {{"kind", "deactivated"}, {"metadata", Metadata(session)}},
                  !session.run_id.empty());
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
    // A fork receipt must not become visible until its new conversation is
    // in the catalogue; clients can inspect it immediately after the receipt.
    if (JsonValue(JsonValue(frame, "result", json::object()), "forked",
                  false)) {
      lock.unlock();
      RefreshCatalogue(true);
      lock.lock();
    }
    if (!outcomes_.ResolveOutcome(*session, frame)) {
      if (!session->run_id.empty() && !JsonValue(frame, "accepted", false)) {
        session->error =
            JsonValue(frame, "error", "scheduled submission failed");
        session->run_result = "failed";
      }
      return;
    }
  } else {
    ApplyRuntimeFrame(*session, frame);
  }
  replay_.Publish(epoch_, session->id, session->generation, std::move(frame),
                  !session->run_id.empty());
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
                         ReadWorkerBinary(session->path))) {
    return false;
  }
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
        outcomes_.FailPending(*session);
        session->turn_active = false;
        session->pending = nullptr;
        if (session->closing) {
          DeactivateLocked(*session);
        } else {
          session->status = "interrupted";
          session->state["activity"] = "Interrupted";
          replay_.Publish(epoch_, session->id, session->generation,
                          {{"kind", "closed"}}, !session->run_id.empty());
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
    replay_.Publish(epoch_, session->id, session->generation,
                    {{"kind", "activated"}, {"metadata", Metadata(*session)}},
                    !session->run_id.empty());
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
            session.run_result =
                RunResultFor(JsonValue((*it)["summary"], "outcome", "error"));
            break;
          }
        }
      }
      if (!session.run_result.empty()) session.run_checkpoint = true;
      session.live_truncated = false;
      session.active_exchanges.clear();
      session.updated = NowMillis();
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
      session.run_result = RunResultFor(outcome);
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

}  // namespace uagent::session
