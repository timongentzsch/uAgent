// Copyright 2026 Timon Gentzsch

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/tools/child_agent.h"
#include "include/tools/jobs.h"

namespace uagent {
namespace {

std::string ActivityCount(size_t count) {
  return std::to_string(count) + (count == 1 ? " activity" : " activities");
}

}  // namespace

// Drain finished activities exactly once after the process-I/O owner has
// recorded status and drained trailing output.
namespace {

// A subagent is named by its kind because its command line is harness plumbing;
// every other activity is identified by what it was asked to run.
std::string ResultHeader(ActivityKind kind, int64_t id,
                         const std::string& command) {
  if (kind == ActivityKind::kSubagent) {
    return "[Background result: subagent id " + std::to_string(id) + "]";
  }
  return "[" +
         std::string(kind == ActivityKind::kDetached ? "Detached"
                                                     : "Background") +
         " result: activity id " + std::to_string(id) + " `" +
         FirstLine(command) + "`]";
}

}  // namespace

std::string BgResultHeader(const BgJob& job) {
  return ResultHeader(job.kind, ActivityId(job), job.cmd);
}

std::string BgResultHeader(const BackgroundCompletion& completion) {
  return ResultHeader(completion.kind, completion.activity_id,
                      completion.command);
}

namespace {

int64_t AutomaticResultCap() {
  int64_t cap = ToolResultCap();
  return cap > 0 ? std::min<int64_t>(cap, kActivityResultChars)
                 : kActivityResultChars;
}

std::vector<std::string> TakeCompleted(
    ProcessSupervisor& supervisor, std::string_view kind,
    const std::vector<int64_t>* ids, std::vector<BackgroundCompletion>* details,
    int64_t output_cap) {
  std::vector<BgJob> jobs = supervisor.Snapshot();
  std::vector<std::string> notes;
  for (BgJob& candidate : jobs) {
    // Filter before locking: both tests read fields that are immutable once the
    // id is assigned, and `interaction` is held for a whole tool interaction,
    // so taking it first blocks this wait on an unrelated activity.
    if (ids && std::find(ids->begin(), ids->end(), ActivityId(candidate)) ==
                   ids->end()) {
      continue;
    }
    if (!kind.empty() &&
        candidate.kind != ParseActivityKind(std::string(kind))) {
      continue;
    }
    std::unique_lock<std::mutex> interaction;
    if (candidate.session) {
      interaction =
          std::unique_lock<std::mutex>(candidate.session->interaction);
    }

    int status = 0;
    bool completed = false;
    if (candidate.detached) {
      pid_t waited = WaitPid(candidate.pid, &status, WNOHANG);
      bool leader_reaped =
          waited == candidate.pid || (waited < 0 && errno == ECHILD);
      completed = leader_reaped && !ProcessGroupAlive(candidate.pid);
      if (!completed) continue;
    } else if (candidate.session) {
      std::lock_guard<std::mutex> lock(candidate.session->mutex);
      completed = candidate.session->state == ActivityState::kDrained;
      status = candidate.session->wait_status.value_or(0);
      if (!completed) continue;
    }

    std::optional<BgJob> taken = supervisor.Take(ActivityId(candidate));
    if (!taken) continue;  // another waiter owns exactly-once delivery
    BgJob job = std::move(*taken);
    if (!job.detached) BgTrackSignal(job.pid, false);
    if (job.detached) unlink(DetachedRecordPath(job.pid).c_str());
    std::string incremental =
        job.session ? DrainActivityOutput(job, output_cap) : std::string();
    bool failed = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CollectedLog collected =
        job.detached
            ? CollectedLog{ReadLogTail(job.log, output_cap), std::nullopt}
            : CollectCompletedLog(job.log, output_cap, failed);
    if (job.detached) RemoveLog(job.log);
    std::string output;
    if (job.session) {
      output = incremental.empty() || incremental == kNoNewActivityOutput
                   ? std::string(kNoNewActivityOutput)
                   : std::move(incremental);
    } else {
      output = std::move(collected.output);
    }
    // Appended after any failure report, not before: inside it the hint would
    // be squeezed out with the rest of the diagnostics, and the pointer to the
    // full log is the one line that must survive.
    std::string artifact_note =
        collected.artifact ? ArtifactHint(*collected.artifact) : std::string();
    ActivityKind activity_kind = job.kind;
    if (activity_kind == ActivityKind::kSubagent) {
      output = ChildAgentRecoverEnvelope(
          std::move(output),
          collected.artifact ? collected.artifact->path : job.log);
      if (failed) {
        output = ChildAgentFailureReport(
            job.display_label, ChildAgentFailureStage::kExecution, output);
        output += ChildAgentConstraintNotes(job.completion_notes);
      } else {
        output = ChildAgentAnswer(std::move(output), job.completion_notes);
      }
      if (!job.source_id.empty()) {
        output += "\n[collaborator " + job.source_id +
                  "; resume with subagent operation=followup]";
      }
    }
    output += artifact_note;
    std::string formatted =
        BgResultHeader(job) + "\n" + output + FmtExit(status, /*show_ok=*/true);
    notes.push_back(formatted);
    if (details) {
      details->push_back({ActivityId(job), activity_kind, status, job.cmd,
                          std::move(output), job.display_label,
                          job.receipt_path, job.source_id});
    }
    if (!job.detached) supervisor.Retain(std::move(job));
  }
  return notes;
}

}  // namespace

std::vector<std::string> BgTakeCompleted(ProcessSupervisor& supervisor,
                                         std::string_view kind) {
  return TakeCompleted(supervisor, kind, nullptr, nullptr,
                       AutomaticResultCap());
}

std::vector<BackgroundCompletion> BgTakeCompletedDetails(
    ProcessSupervisor& supervisor, std::string_view kind) {
  std::vector<BackgroundCompletion> details;
  (void)TakeCompleted(supervisor, kind, nullptr, &details,
                      AutomaticResultCap());
  return details;
}

ToolResult ToolActivityWait(ProcessSupervisor& supervisor,
                            const std::vector<int64_t>& requested,
                            std::string_view mode, int64_t wait_ms,
                            const ToolContext& context,
                            int64_t max_output_chars) {
  // Waiting consumes the completion it observes. Memory extraction is drained
  // by the harness into the memory audit instead, so a wait that scooped one
  // up would silently lose that record; it is no more waitable than a detached
  // activity.
  auto waitable = [](const BgJob& job) {
    return !job.detached && job.kind != ActivityKind::kMemory;
  };
  std::vector<int64_t> ids;
  if (requested.empty()) {
    for (const BgJob& job : supervisor.Snapshot()) {
      if (waitable(job)) ids.push_back(ActivityId(job));
    }
  } else {
    for (int64_t requested_id : requested) {
      std::optional<BgJob> job =
          requested_id > 0 ? supervisor.Find(requested_id) : std::nullopt;
      if (!job) {
        return ToolFailure(ToolErrorCode::kNotFound,
                           "error: activity " + std::to_string(requested_id) +
                               " is not running in this session");
      }
      if (!waitable(*job)) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "error: activity " + std::to_string(requested_id) +
                               " is harness maintenance and is not waitable");
      }
      if (std::find(ids.begin(), ids.end(), requested_id) == ids.end()) {
        ids.push_back(requested_id);
      }
    }
  }
  if (ids.empty()) return ToolSuccess("(no waitable activities running)");

  TerminalActivityLabel waiting(
      ids.size() == 1 ? "wait · activity " + std::to_string(ids.front())
                      : "wait · " + ActivityCount(ids.size()));

  int64_t cap = ActivityOutputCap(max_output_chars);

  // The caller's wait_ms is only half the story: a tool call may not outlive
  // context.deadline, which for this tool is the turn's rather than the
  // per-call budget. Both ends are reported below, because "timed out" alone
  // reads as though the requested wait elapsed and invites the caller to give
  // up on an activity that is running normally.
  auto wait_started = std::chrono::steady_clock::now();
  auto wait_requested = wait_started + std::chrono::milliseconds(wait_ms);
  auto deadline = std::min(context.deadline, wait_requested);
  bool wait_capped = context.deadline < wait_requested;
  std::string output;
  for (;;) {
    uint64_t generation = supervisor.Generation();
    std::vector<std::string> completed =
        TakeCompleted(supervisor, {}, &ids, nullptr, cap);
    for (std::string& note : completed) {
      if (!output.empty()) output += "\n\n";
      output += note;
    }
    size_t running = static_cast<size_t>(
        std::count_if(ids.begin(), ids.end(),
                      [&](int64_t id) { return supervisor.IsLive(id); }));
    if ((mode == "any" && !completed.empty()) || running == 0) {
      std::string result =
          output.empty() ? "(activities already complete)" : std::move(output);
      return ToolSuccess(LimitOutput(std::move(result), cap));
    }
    if (AbortRequested()) {
      return ToolCancelled("wait interrupted; " + ActivityCount(running) +
                           " still running");
    }
    if (SteeringYieldRequested()) {
      if (!output.empty()) output += "\n\n";
      output += "[wait yielded for queued steering; " + ActivityCount(running) +
                " still running]";
      return ToolSuccess(LimitOutput(std::move(output), cap));
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (!output.empty()) output += "\n\n";
      double waited = std::chrono::duration<double>(now - wait_started).count();
      output += "[waited " + FmtDuration(waited);
      if (wait_capped) {
        output += " of " + FmtDuration(static_cast<double>(wait_ms) / 1000.0) +
                  " requested, capped by the turn deadline";
      }
      output += "; " + ActivityCount(running) +
                " still running; call again to keep waiting]";
      return ToolSuccess(LimitOutput(std::move(output), cap));
    }
    // Process state changes, Escape, and queued steering all pair with Wake(),
    // so this predicate wait needs no periodic abort polling.
    supervisor.WaitForChange(generation, deadline);
  }
}

}  // namespace uagent
