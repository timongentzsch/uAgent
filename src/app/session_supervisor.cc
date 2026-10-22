// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/session_view.h"
#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/app/session.h"
#include "include/app/session_host.h"
#include "include/core/activity.h"
#include "include/core/capture.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
namespace {
// The status a connected worker reports through its latest state/activity.
SessionStatus LiveStatus(const HostSession& session) {
  return session.closing                    ? SessionStatus::kClosing
         : !session.pending.is_null()       ? SessionStatus::kWaiting
         : !session.state.contains("route") ? SessionStatus::kStarting
         : session.turn_active              ? SessionStatus::kRunning
         : session.command_busy             ? SessionStatus::kProcessing
                                            : SessionStatus::kIdle;
}

// Sets next["view"]: the held view merged with what the worker sent.
// Returns the rows that changed, or null when a browser holding the view
// must take all of it: a new view epoch, or a field of the view that changed
// other than by rows bounded out (a browser keeps those as older pages).
json MergeView(HostSession& session, json& next) {
  // A rewound conversation starts a new view epoch: its old blocks are
  // gone, so the checkpoint replaces the view instead of merging into it.
  json view = json::object();
  auto held = session.state.find("view");
  const bool same = held != session.state.end() &&
                    JsonValue(next, "view_epoch", uint64_t{0}) ==
                        JsonValue(session.state, "view_epoch", uint64_t{0});
  if (same) view = std::move(*held);
  json fields = json::object();
  for (auto it = view.begin(); it != view.end(); ++it) {
    if (it.key() != "blocks") fields[it.key()] = it.value();
  }
  json rows = json::array();
  size_t dropped = 0;
  if (const json* checkpoint_view = JsonObject(next, "view")) {
    for (auto it = checkpoint_view->begin(); it != checkpoint_view->end();
         ++it) {
      if (it.key() != "blocks") view[it.key()] = it.value();
    }
    if (const json* blocks = JsonArray(*checkpoint_view, "blocks")) {
      dropped = MergeDisplayBlocks(view, *blocks, &rows);
    }
  }
  if (!view.contains("blocks")) view["blocks"] = json::array();
  bool whole = !same;
  for (auto it = view.begin(); it != view.end() && !whole; ++it) {
    // Rows bounded out move `before` and `more`; a browser keeps its own.
    const bool bounded =
        dropped && (it.key() == "before" || it.key() == "more");
    whole = it.key() != "blocks" && !bounded &&
            JsonValue(fields, it.key().c_str(), json()) != it.value();
  }
  next["view"] = std::move(view);
  return whole ? json(nullptr) : rows;
}

// The tool result a browser may ask to see in full, kept while it is recent.
void RetainExchange(HostSession& session, const json& data) {
  const std::string detail = JsonValue(data, "detail_id", "");
  if (detail.empty()) return;
  if (session.active_exchanges.size() >= kMaxActiveExchanges) {
    session.active_exchanges.erase(session.active_exchanges.begin());
  }
  session.active_exchanges[detail] = {
      {"request",
       {{"name", JsonValue(data, "name", "")},
        {"arguments", "retained with tool call"}}},
      {"response", JsonValue(data, "result", "")},
      {"complete", true}};
}
}  // namespace

std::string SessionHost::RunResultFor(const std::string& outcome) {
  return outcome == "complete"      ? "completed"
         : outcome == "interrupted" ? "interrupted"
                                    : "failed";
}

std::shared_ptr<HostSession> SessionHost::CreateSession(
    const std::string& cwd, const std::string& path, const std::string& title,
    std::string& error) {
  if (sessions_.size() >= kMaxSessions) {
    error = "session catalogue limit reached";
    return {};
  }
  if (!EnsurePrivateDirectory(
          std::filesystem::path(path).parent_path().string()) ||
      !EnsurePrivateDirectory(directory_ + "/" + kDraftsDir)) {
    error = "cannot create private session directory";
    return {};
  }
  auto session = std::make_shared<HostSession>();
  session->cwd = cwd;
  session->path = path;
  session->id = HashHex(path);
  session->title = title.empty() ? "New conversation" : title;
  session->draft_title = title;
  session->status = SessionStatus::kDraft;
  session->updated = NowMillis();
  if (!WriteDraft(*session, title, session->updated)) {
    error = "cannot persist draft identity";
    return {};
  }
  sessions_[session->id] = session;
  return session;
}

bool SessionHost::WriteDraft(const HostSession& session,
                             const std::string& title, int64_t updated) const {
  return ToolWritePrivateFile(
             directory_ + "/" + kDraftsDir + "/" + session.id + ".json",
             JsonDump({{"cwd", session.cwd},
                       {"path", session.path},
                       {"id", session.id},
                       {"title", title},
                       {"updated", updated}}))
      .Ok();
}

bool SessionHost::IsCurrentLocked(const HostSession* session) const {
  auto owner = sessions_.find(session->id);
  return owner != sessions_.end() && owner->second.get() == session;
}

void SessionHost::PublishLifecycle(const HostSession& session, json frame) {
  frame["metadata"] = Metadata(session);
  replay_.Publish(epoch_, session.id, session.generation, std::move(frame),
                  !session.run_id.empty());
}

Connection SessionHost::OpenRuntime(const HostSession& session, bool create,
                                    std::string& error) const {
  Options options;
  if (!session.launch.empty()) {
    const std::string model = JsonValue(session.launch, "model", "");
    if (!model.empty()) options.overrides["UAGENT_MODEL"] = model;
    options.overrides["UAGENT_APPROVAL"] =
        JsonValue(session.launch, "permissions", "ask");
  }
  Connection connection = create ? Open(executable_, session.cwd, session.path,
                                        session.draft_title, options, error)
                                 : Connect(session.path);
  return connection;
}

void SessionHost::DeactivateLocked(HostSession& session) {
  session.closing = false;
  session.pid = -1;
  {
    std::lock_guard send_lock(session.send_mutex);
    session.generation.clear();
    session.socket.Reset();
  }
  session.status = SessionStatus::kSaved;
  session.error.clear();
  // A saved conversation is read from disk.
  session.state = {{"activity", kIdleActivity}};
  PublishLifecycle(session, {{"kind", "deactivated"}});
}

void SessionHost::Received(HostSession* session, json frame) {
  UniqueLock lock(mutex_);
  if (!IsCurrentLocked(session) ||
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
  if (kind == "retiring") {
    // The runtime has nothing to do and stops once nobody holds it: let go.
    session->closing = true;
    session->stop.Wake();
    return;
  }
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
    if (!ApplyRuntimeFrame(*session, frame)) return;
    // Clients adopt the host's lifecycle status rather than re-deriving it.
    if (kind == "state" || kind == "activity") {
      frame["metadata"] = Metadata(*session);
    }
  }
  // A scheduled run or a pending restart wakes the scheduler, which acts
  // once the turn has ended.
  replay_.Publish(epoch_, session->id, session->generation, std::move(frame),
                  !session->run_id.empty() || session->restart);
  if (kind == "gap") {
    lock.unlock();
    session->Send(ControlFrame("refresh"));
  }
  changed_.notify_all();
}

bool SessionHost::RecycleStaleWorkerLocked(
    const std::shared_ptr<HostSession>& session, UniqueLock& lock) {
  // A worker from another build of the executable is recycled, and so is one
  // asked to restart; one whose executable cannot be stated is left alone.
  const std::string current = FileIdentity(executable_);
  if ((current.empty() || current == session->binary) && !session->restart) {
    return false;
  }
  // Binary upgraded since this worker spawned: graceful close, then the
  // caller spawns fresh. Same semantics as user-initiated close of a busy
  // session; a worker that ignores close keeps serving (fail open, retried
  // on the next touch).
  DebugLog("worker_binary_recycle", {{"session", session->id}});
  // Claimed now, so no other pass starts a second recycle while this one
  // waits; restored if the worker keeps running.
  const bool restart = std::exchange(session->restart, false);
  session->Send(ControlFrame("close"));
  changed_.wait_for(lock, kWorkerShutdownTimeout,
                    [&] { return session->exited.load(); });
  if (!session->exited) {
    session->restart = restart;
    return false;
  }
  session->pid = -1;
  if (session->reader.joinable()) {
    mutex_.unlock();
    session->reader.join();
    mutex_.lock();
  }
  return true;
}

bool SessionHost::ActivateLocked(const std::shared_ptr<HostSession>& session,
                                 std::string& error, UniqueLock& lock,
                                 bool create) {
  if (session->closing) {
    error = "session is closing";
    return false;
  }
  bool create_now = create;
  for (int attempt = 0;; ++attempt) {
    // A command that arrives while another starts the runtime waits for that
    // start: it then finds the runtime, or starts it itself.
    if (session->connecting) {
      changed_.wait(lock, [&] { return !session->connecting || stopping_; });
      if (stopping_ || !IsCurrentLocked(session.get())) {
        error = "session was closed while starting";
        return false;
      }
    }
    if (session->pid > 0 && !session->exited) {
      if (!RecycleStaleWorkerLocked(session, lock)) return true;
      create_now = true;  // recycled: fall through to a fresh spawn
    }
    session->connecting = true;
    if (session->reader.joinable()) {
      mutex_.unlock();
      session->reader.join();
      mutex_.lock();
      if (stopping_ || !IsCurrentLocked(session.get())) {
        session->connecting = false;
        changed_.notify_all();
        error = "session was closed while joining its prior runtime";
        return false;
      }
    }
    mutex_.unlock();
    auto connected = OpenRuntime(*session, create_now, error);
    mutex_.lock();
    session->connecting = false;
    changed_.notify_all();
    if (stopping_ || !IsCurrentLocked(session.get())) {
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
    session->binary = std::move(connected.binary);
    session->exited = false;
    session->status = SessionStatus::kStarting;
    session->error.clear();
    session->reader = std::thread([this, session = session.get()] {
      ReadFrames(session->socket.Get(), session->stop.read.Get(), kFrameBytes,
                 [&](json frame) {
                   Received(session, std::move(frame));
                   return !stopping_;
                 });
      std::lock_guard state_lock(mutex_);
      if (IsCurrentLocked(session)) {
        outcomes_.FailPending(*session);
        // Only a runtime that ends in the middle of work was interrupted;
        // one that ends with nothing under way (closed by any client, or
        // idle) is simply saved.
        const bool working = session->turn_active || session->command_busy;
        session->turn_active = false;
        session->pending = nullptr;
        if (session->closing || !working) {
          DeactivateLocked(*session);
        } else {
          session->status = SessionStatus::kInterrupted;
          session->state["activity"] = "Interrupted";
          PublishLifecycle(*session, {{"kind", "closed"}});
        }
      }
      session->exited = true;
      changed_.notify_all();
    });
    // Adopted a live worker through Connect: it may predate the executable
    // (host restarted over it). Recycle through the same gate, then spawn.
    // A fresh spawn reports the executable on disk and skips this.
    if (RecycleStaleWorkerLocked(session, lock)) {
      if (attempt > 0) {
        error = "worker binary changed during activation";
        return false;
      }
      create_now = true;
      continue;
    }
    session->activated = NowMillis();
    PublishLifecycle(*session, {{"kind", "activated"}});
    return true;
  }  // for (attempt): single pass unless a stale worker recycled above
}

void SessionHost::ApplyStateFrame(HostSession& session, json& frame) {
  auto sent = frame.find("state");
  json next = sent != frame.end() ? std::move(*sent) : json::object();
  json rows = MergeView(session, next);
  // Between checkpoints the worker sends only light state: keep the rest.
  std::vector<const char*> resent;
  for (const char* field : session::kCheckpointFields) {
    if (!next.contains(field)) {
      if (session.state.contains(field)) {
        next[field] = std::move(session.state[field]);
      }
    } else if (std::string_view(field) != "view" &&
               JsonValue(session.state, field, json()) != next[field]) {
      resent.push_back(field);
    }
  }
  session.state = std::move(next);
  // Browsers hold the host's view, never the worker's partial one, and
  // apply block patches to it between checkpoints. A checkpoint reaches
  // them the same way: the rows that changed, then its state without the
  // view and with only the checkpoint fields that changed. The whole view
  // goes only when they could not follow it row by row.
  const bool checkpoint = JsonValue(frame, "checkpoint", false);
  const bool whole = checkpoint && rows.is_null();
  frame["state"] = whole ? session.state : session::LightState(session.state);
  if (checkpoint && !whole) {
    for (const char* field : resent) {
      frame["state"][field] = session.state[field];
    }
    for (json& row : rows) {
      replay_.Publish(epoch_, session.id, session.generation,
                      {{"kind", "block"}, {"block", std::move(row)}},
                      !session.run_id.empty() || session.restart);
    }
  }
  session.pending = JsonValue(frame, "pending", json(nullptr));
  session.turn_active = JsonValue(frame, "busy", false);
  session.command_busy = JsonValue(frame, "command_busy", false);
  session.guidance = JsonValue(frame, "guidance", uint64_t{0});
  session.status = LiveStatus(session);
  if (checkpoint) {
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
    // A member is called what its session is.
    if (!title.empty() && session.kind == kSessionKindThread &&
        !session.member.empty()) {
      session.member = title;
    }
  }
  frame["updated"] = session.updated;
}

// What a run learns from the events of its turns.
void SessionHost::RecordRunEvent(HostSession& session, const std::string& type,
                                 json& frame) {
  if (!session.run_id.empty()) {
    if (type == AppType(EventId::kTurnCompleted)) {
      const std::string outcome = JsonValue(frame["data"], "outcome", "error");
      session.run_result = RunResultFor(outcome);
      if (session.run_result == "failed") {
        session.error = "Turn ended: " + outcome;
      }
    } else if (type == AppType(EventId::kTurnStopped)) {
      session.run_result = "interrupted";
    } else if (type == AppType(EventId::kError)) {
      session.error = JsonValue(frame["data"], "error", "scheduled run failed");
    }
  }
}

bool SessionHost::ApplyRuntimeFrame(HostSession& session, json& frame) {
  const std::string kind = JsonValue(frame, "kind", "");
  if (kind == "state") {
    ApplyStateFrame(session, frame);
    return true;
  }
  if (kind == "activity") {
    session.state["activity"] = JsonValue(frame, "activity", kIdleActivity);
    session.state["phase"] = JsonValue(frame, "phase", "working");
    session.turn_active = JsonValue(frame, "busy", false);
    session.status = LiveStatus(session);
    return true;
  }
  if (kind == "gap") {
    session.live_truncated = true;
    return true;
  }
  if (kind == "error") {
    session.error = JsonValue(frame, "error", "worker failed");
    session.status = SessionStatus::kFailed;
    return true;
  }
  if (kind != "event") return true;
  const std::string type = JsonValue(frame, "type", "");
  // Terminal renderings of what the transcript rows already show; terminals
  // read them from the worker, browsers never do.
  if (type == AppType(EventId::kPresentation)) {
    return false;
  }
  RecordRunEvent(session, type, frame);
  json patch;
  ApplySessionEvent(session.state, type, frame["data"], &patch);
  if (type == AppType(EventId::kToolResult)) {
    RetainExchange(session, frame["data"]);
  }
  if (type == AppType(EventId::kMessageChanged)) {
    const json& block = frame["data"]["block"];
    session.incoming =
        std::max(session.incoming, JsonValue(block, "incoming", uint64_t{0}));
  }
  // Browsers receive what changed in the host's view instead of the events
  // that changed it, so they apply rows and never re-derive them.
  if (type == AppType(EventId::kResponseStarted) ||
      type == AppType(EventId::kAnswerDelta) ||
      type == AppType(EventId::kReasoningDelta) ||
      type == AppType(EventId::kResponseFinished) ||
      type == AppType(EventId::kToolCall) ||
      type == AppType(EventId::kToolResult) ||
      type == AppType(EventId::kMessageChanged)) {
    if (patch.is_null()) return false;
    patch["time"] = frame["time"];
    frame = std::move(patch);
  }
  return true;
}

}  // namespace uagent::session
