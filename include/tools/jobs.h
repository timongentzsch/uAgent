// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_JOBS_H_
#define UAGENT_INCLUDE_TOOLS_JOBS_H_
// Bounded job-log, detached-terminal, and process lifecycle declarations.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

// The activity tool's slice of the global result cap, shared by the tool's own
// budget and by the automatic background-completion text. Background output is
// observational, so it stays well under what a foreground read gets.
inline constexpr int64_t kActivityResultChars = 6000;

struct CollectedLog {
  std::string output;
  std::optional<ToolArtifact> artifact;
};

struct BackgroundCompletion {
  int64_t activity_id = 0;
  ActivityKind kind = ActivityKind::kCommand;
  std::string kind_label;  // the spawning job_kind, e.g. subagent
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
void KillProcess(pid_t pid);
std::string FmtExit(int status, bool show_ok);
ToolResult ProcessResult(std::string output, int status);
// Shared output budgeting for every activity read: ActivityOutputCap folds a
// caller request into the global cap, LimitOutput enforces it head-and-tail.
int64_t ActivityOutputCap(int64_t requested);
std::string LimitOutput(std::string text, int64_t cap);
ToolResult LimitOutput(ToolResult result, int64_t cap);
std::string ReadLogTail(const std::string& path, int64_t cap);
uint64_t LogFileBytes(const std::string& path);
void RemoveLog(const std::string& path);
ToolArtifact PromoteLogArtifact(const std::string& path, uint64_t bytes);
CollectedLog CollectCompletedLog(const std::string& path, int64_t cap);
int ToolLogPump(const std::string& path, int64_t max_bytes);
bool ProcessGroupAlive(pid_t leader);
std::string DetachedRecordPath(pid_t pid);
std::vector<json> DetachedRecords();
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

#endif  // UAGENT_INCLUDE_TOOLS_JOBS_H_
