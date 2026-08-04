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

struct CollectedLog {
  std::string output;
  std::optional<ToolArtifact> artifact;
};

void BgTrackSignal(pid_t pid, bool add);
void KillProcess(pid_t pid, int* status = nullptr);
std::string FmtExit(int status, bool show_ok);
ToolResult ProcessResult(std::string output, int status);
std::string ReadLogTail(const std::string& path, int64_t cap);
uint64_t LogFileBytes(const std::string& path);
void RemoveLog(const std::string& path);
ToolArtifact PromoteLogArtifact(const std::string& path, uint64_t bytes);
CollectedLog CollectCompletedLog(const std::string& path, int64_t cap);
int ToolLogPump(const std::string& path, int64_t max_bytes);
bool ProcessAlive(pid_t pid);
std::string DetachedRecordPath(pid_t pid);
std::vector<json> DetachedRecords();
ToolResult SaveDetachedRecord(pid_t pid, const std::string& log,
                              const std::string& command);
ToolResult ToolTerminalOutput(const ProcessSupervisor& supervisor, int64_t pid);
std::string BgResultHeader(const BgJob& job);
std::vector<std::string> BgTakeCompleted(ProcessSupervisor& supervisor,
                                         std::string_view kind = {});
std::vector<std::string> BgTakeCompletedExcept(ProcessSupervisor& supervisor,
                                               std::string_view kind);
void BgShutdownAll(ProcessSupervisor& supervisor);
size_t BgCancelTasks(ProcessSupervisor& supervisor);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_JOBS_H_
