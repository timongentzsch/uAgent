// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_JOBS_H_
#define UAGENT_INCLUDE_AGENT_JOBS_H_
// Bounded job-log, detached-terminal, and process lifecycle declarations.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/agent/process.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {

// A wait bounded by the caller's request and by the turn deadline. The note
// says which one ended it: "timed out" alone reads as though the requested
// wait elapsed and invites giving up on an activity that is running normally.
struct WaitWindow {
  // Declared first: the constructor measures the deadline from it.
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  int64_t requested_ms;
  std::chrono::steady_clock::time_point deadline;
  bool capped;

  WaitWindow(int64_t wait_ms, std::chrono::steady_clock::time_point turn_end)
      : requested_ms(wait_ms),
        deadline(
            std::min(turn_end, started + std::chrono::milliseconds(wait_ms))),
        capped(turn_end < started + std::chrono::milliseconds(wait_ms)) {}

  // "<lead> 3s", or "<lead> 3s of 10s requested, capped by the turn deadline".
  std::string Note(std::string_view lead) const {
    const double waited = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - started)
                              .count();
    std::string note = std::string(lead) + " " + FmtDuration(waited);
    if (capped) {
      note += " of " + FmtDuration(static_cast<double>(requested_ms) / 1000.0) +
              " requested, capped by the turn deadline";
    }
    return note;
  }
};

// The activity tool's slice of the global result cap, shared by the tool's own
// budget and by the automatic background-completion text. Background output is
// observational, so it stays well under what a foreground read gets.
inline constexpr int64_t kActivityResultChars = 6000;
inline constexpr std::string_view kNoNewActivityOutput = "(no new output)";

struct CollectedLog {
  std::string output;
  std::optional<ToolArtifact> artifact;
};

struct BackgroundCompletion {
  int64_t activity_id = 0;
  ActivityKind kind = ActivityKind::kCommand;
  int status = 0;
  std::string command;
  std::string output;
  std::string display_label;
  std::string receipt_path;
  std::string source_id;
};

struct DetachedActivity {
  pid_t pid;
  std::string log;
};

void BgTrackSignal(pid_t pid, bool add);
bool SignalProcessGroup(pid_t leader, int signal_number);
void KillProcess(pid_t pid);
std::string FmtExit(int status, bool show_ok);
ToolResult ProcessResult(std::string output, int status);
// Shared output budgeting for every activity read: ActivityOutputCap folds a
// caller request into the global cap, LimitOutput enforces it head-and-tail.
int64_t ActivityOutputCap(int64_t requested);
std::string LimitOutput(const std::string& text, int64_t cap);
ToolResult LimitOutput(ToolResult result, int64_t cap);
std::string DrainActivityOutput(const BgJob& job, int64_t cap);
std::string ReadLogTail(const std::string& path, int64_t cap);
uint64_t LogFileBytes(const std::string& path);
void RemoveLog(const std::string& path);
ToolArtifact PromoteLogArtifact(const std::string& path, uint64_t bytes);
// `failed` keeps the log as an artifact whatever its size: a failure is
// summarised on the way back, so deleting the log discards the detail exactly
// when someone needs it.
CollectedLog CollectCompletedLog(const std::string& path, int64_t cap,
                                 bool failed);
int ToolLogPump(const std::string& path, int64_t max_bytes);
bool ProcessGroupAlive(pid_t leader);
std::string DetachedRecordPath(pid_t pid);
std::vector<json> DetachedRecords();
std::optional<json> FindDetachedRecord(int64_t pid);
ToolResult ActivityNotFound(int64_t pid);
std::optional<DetachedActivity> FindRunningDetachedActivity(
    const std::string& command);
ToolResult SaveDetachedRecord(pid_t pid, const std::string& log,
                              const std::string& command);
ToolResult ToolActivityList(const ProcessSupervisor& supervisor);
ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor, int64_t id);
ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor, int64_t id,
                              int64_t wait_ms, std::string_view until,
                              const ToolContext& context,
                              int64_t max_output_chars = 0);
ToolResult ToolActivityInput(const ProcessSupervisor& supervisor, int64_t id,
                             const std::string& chars, int64_t wait_ms,
                             const ToolContext& context, int64_t rows = 0,
                             int64_t cols = 0, int64_t max_output_chars = 0);
ToolResult ToolActivityWait(ProcessSupervisor& supervisor,
                            const std::vector<int64_t>& ids,
                            std::string_view mode, int64_t wait_ms,
                            const ToolContext& context,
                            int64_t max_output_chars = 0);
ToolResult ToolActivityStop(ProcessSupervisor& supervisor, int64_t id);
std::string BgResultHeader(const BgJob& job);
std::string BgResultHeader(const BackgroundCompletion& completion);
std::vector<std::string> BgTakeCompleted(ProcessSupervisor& supervisor,
                                         std::string_view kind = {});
std::vector<BackgroundCompletion> BgTakeCompletedDetails(
    ProcessSupervisor& supervisor, std::string_view kind = {});
void BgShutdownAll(ProcessSupervisor& supervisor);
size_t BgCancelSubagents(ProcessSupervisor& supervisor);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_JOBS_H_
