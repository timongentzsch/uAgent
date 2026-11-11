// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_ENV_H_
#define UAGENT_INCLUDE_CORE_ENV_H_
// Environment lookups and the named readers of settings. The parsed runtime
// configuration is in runtime_config.h.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

std::string EnvStr(const char* name, const std::string& dflt = "");

int64_t EnvLong(const char* name, int64_t dflt);

int64_t ToolResultCap();
int64_t ToolBatchResultCap();
int64_t AutoCompactPct();
int64_t AutoCompactTokens();
// 0 is a conversation nobody started; nesting is bounded by agents.depth.
int64_t AgentDepth();
bool CanDelegate();
int64_t MaxOutputTokens();
// Experiment overlay for the base prompt: a path, empty when unset.
std::string PromptOverlayPath();

int64_t ReadFileLines();
int64_t AttachmentLimitMb();
int64_t HistoryDays();
int64_t HistoryFiles();
int64_t LogKeepDays();
int64_t LogKeepFiles();
int64_t IdleMinutes();
// Commands one conversation runs at once.
int64_t MaxActivities();
int64_t ToolConcurrency();
// Byte budgets of the file tools; the registry states them in KiB and MiB.
size_t ReadFileBytes();
// read_path's result allowance covers its header lines.
int64_t ReadFileResultChars();
size_t EditFileBytes();
int64_t ActivityLogBytes();
int64_t ListDirEntries();
int64_t GrepResults();
int64_t WebSearchMaxResults();
int64_t WebSearchMaxUses();
int64_t WebSearchCalls();
int64_t WebSearchTimeoutSeconds();
size_t MaxMemories();
size_t MemoryBytes();
size_t MemoryAlwaysBytes();
int64_t MemoryIdleSeconds();
size_t MaxSkills();
size_t ProjectDocBytes();
int64_t MaxMcpServers();
size_t MaxMcpTools();
size_t MaxPendingAttachments();
int MaxImageDimension();
// Identical consecutive calls that end a turn.
int64_t RepeatedCallStopAfter();

// A running session can change the approval mode, so it cannot live in
// environ, which a spawn reads on another thread; children get it as an
// override. The modes are in kApprovalModes' order.
enum class ApprovalMode { kAsk, kAuto, kYolo, kPlan };
const char* ApprovalModeName(ApprovalMode mode);
bool ParseApprovalMode(std::string_view value, ApprovalMode& mode);
ApprovalMode CurrentApprovalMode();
void SetApprovalMode(ApprovalMode mode);
bool ApprovalIsYolo();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_ENV_H_
