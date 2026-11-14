// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_ACTIVITY_H_
#define UAGENT_INCLUDE_AGENT_ACTIVITY_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {

// Background output is observational, so it gets less than a foreground read.
inline constexpr int64_t kActivityResultChars = 6000;

struct CollectedLog {
  std::string output;
  std::optional<ToolArtifact> artifact;
};

struct ActivityCompletion {
  int64_t activity_id = 0;
  ActivityKind kind = ActivityKind::kCommand;
  int status = 0;
  std::string command;
  std::string output;
  std::string hint;  // for the model only: the full log, how to resume it
  std::string display_label;
  int64_t started_ms = 0;  // Unix milliseconds
};

struct DetachedActivity {
  pid_t pid;
  std::string log;
};

void ActivityTrackSignal(pid_t pid, bool add);
// TERM, then KILL after `grace`; false only when the group survives both.
bool TerminateGroup(ActivitySupervisor& supervisor, pid_t leader,
                    std::chrono::milliseconds grace, bool reap_leader);
void KillProcess(pid_t pid);
// Folds a caller's request into the global cap.
int64_t ActivityOutputCap(int64_t requested);
// Keeps the head and the tail within `cap`.
std::string LimitOutput(std::string text, int64_t cap);
ToolResult LimitOutput(ToolResult result, int64_t cap);
std::string ReadLogTail(const std::string& path, int64_t cap);
void RemoveLog(const std::string& path);
// `failed` keeps the log as an artifact whatever its size.
CollectedLog CollectCompletedLog(const std::string& path, int64_t cap,
                                 bool failed);
// The same decision without reading the log.
std::optional<ToolArtifact> KeepCompletedLog(const std::string& path,
                                             int64_t cap, bool failed);
// Copies `input` to a log of two rotating halves until EOF or `stop`.
int PumpLog(int input, const std::string& path, int64_t max_bytes,
            const std::stop_token& stop = {});
int ToolLogPump(const std::string& path, int64_t max_bytes);
bool ProcessGroupAlive(pid_t leader);
std::string DetachedRecordPath(pid_t pid);
std::vector<json> DetachedRecords();
std::optional<DetachedActivity> FindRunningDetachedActivity(
    const std::string& command);
ToolResult SaveDetachedRecord(pid_t pid, const std::string& log,
                              const std::string& command);
ToolResult ToolActivityOutput(const ActivitySupervisor& supervisor, int64_t id);
ToolResult ToolActivityOutput(const ActivitySupervisor& supervisor, int64_t id,
                              int64_t wait_ms, std::string_view until,
                              const ToolContext& context,
                              int64_t max_output_chars = 0);
ToolResult ToolActivityInput(const ActivitySupervisor& supervisor, int64_t id,
                             const std::string& chars, int64_t wait_ms,
                             const ToolContext& context, int64_t rows = 0,
                             int64_t cols = 0, int64_t max_output_chars = 0);
ToolResult ToolActivityWait(ActivitySupervisor& supervisor,
                            const std::vector<int64_t>& ids,
                            std::string_view mode, int64_t wait_ms,
                            const ToolContext& context,
                            int64_t max_output_chars = 0);
ToolResult ToolActivityStop(ActivitySupervisor& supervisor, int64_t id);
std::string ActivityResultHeader(const ActivityCompletion& completion);
// A completion as the model reads it, around `body` (its output or a cut).
std::string ActivityCompletionText(const ActivityCompletion& completion,
                                   std::string_view body);
// A completion as its call's row takes it.
EndedCommand EndedOf(const ActivityCompletion& completion);
std::vector<ActivityCompletion> ActivityTakeCompletedDetails(
    ActivitySupervisor& supervisor, std::string_view kind = {});
void ActivityShutdownAll(ActivitySupervisor& supervisor);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_ACTIVITY_H_
