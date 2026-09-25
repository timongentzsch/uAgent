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
std::chrono::steady_clock::time_point SessionHost::NextScheduleDeadline()
    const {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(24);
  const int64_t now = NowSeconds();
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

bool SessionHost::RefreshScheduleCacheLocked() {
  const FileStamp schedule = SnapshotFile(SchedulePath());
  if (schedule == schedule_stamp_) return false;
  schedule_stamp_ = schedule;
  schedule_state_ = ReadSchedules();
  scheduled_view_ = ScheduleControl({{"action", "list"}});
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
    // Work the run still waits on. A detached process (a server the task
    // started to leave running) outlives its session by design, so it never
    // holds the run open.
    bool background = false;
    if (const json* activities = JsonArray(session->state, "activities")) {
      for (const json& activity : *activities) {
        const std::string state =
            JsonValue(activity, "status", JsonValue(activity, "state", ""));
        if (!JsonValue(activity, "detached", false) &&
            (state == "running" || state == "starting" || state == "stopping" ||
             state == "finishing")) {
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
  const int64_t now = NowSeconds();
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

HostWaitState SessionHost::RunSchedules(bool& recovered) {
  HostWaitState wait;
  ScheduleTick tick;
  std::vector<std::string> projects;
  {
    std::unique_lock lock(mutex_);
    std::vector<std::shared_ptr<HostSession>> recovery;
    if (!recovered) recovered = RecoverSchedules(recovery);
    // External writers (CLI schedule save/run) change the store under us:
    // refresh before the tick so their runs are claimed this iteration,
    // and announce the change like the invalidation path does.
    if (RefreshScheduleCacheLocked()) {
      replay_.Publish(
          epoch_, "", "",
          json{{"kind", "scheduled.changed"}, {"scheduled", scheduled_view_}},
          false);
    }
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
      if (JsonValue(event, "kind", "") == "scheduled.changed") {
        // A write landed during the tick above: re-loop immediately so the
        // fresh cache is claimed instead of sleeping through it.
        wait.deadline =
            std::min(wait.deadline, std::chrono::steady_clock::now());
      }
      replay_.Publish(epoch_, "", "", std::move(event), false);
    }
    // The CLI may queue a run after this tick, before the file watcher opens.
    // Compare against the store version actually processed by this host.
    wait.observed.emplace(SchedulePath(), schedule_stamp_);
  }
  wait.paths = InvalidationPaths(projects);
  return wait;
}

}  // namespace uagent::session
