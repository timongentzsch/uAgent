// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_ENV_H_
#define UAGENT_INCLUDE_CORE_ENV_H_
// Environment lookups and the named readers of settings. The parsed runtime
// configuration is in runtime_config.h, so reading a setting does not pull
// the whole registry into a translation unit.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

std::string EnvStr(const char* name, const std::string& dflt = "");

int64_t EnvLong(const char* name, int64_t dflt);

// Accessors shared by their consumers and the session_ready diagnostic.
int64_t ToolResultCap();
int64_t ToolBatchResultCap();
int64_t AutoCompactPct();
int64_t AutoCompactTokens();
// Delegation depth: 0 is the interactive coordinator. A subagent may delegate
// again while it stays under the cap, so nesting is bounded, not banned.
int64_t AgentDepth();
bool CanDelegate();
bool LeanToolset();
// Budgets handed to a delegated child. Lower than the coordinator's own, so one
// flailing subagent cannot spend the whole turn.
int64_t SubagentMaxSteps();
int64_t SubagentMaxToolCalls();
int64_t SubagentTimeoutSeconds();
int64_t MaxOutputTokens();
bool SandboxEnabled();
// Outbound TCP. Allowed by default: git, npm and pip all need it.
bool SandboxNetworkAllowed();
std::string SandboxWriteRoots();
bool AdaptiveSystemEnabled();
bool MarkdownEnabled();
bool TrustProjectConfig();
// A delegated child echoes one line per durable event to stderr, which is the
// only way its parent can tell work from a stall before the answer arrives.
bool HeadlessProgressEnabled();
// Experiment overlay for the base prompt: a path, empty when unset.
std::string PromptOverlayPath();

// Bounded tunables. Fixed ceilings live in include/core/limits.h.
int64_t ReadFileLines();
int64_t AttachmentLimitMb();
int64_t ContextWindow();
int64_t HistoryDays();
std::string ShellEnvironmentAllowlist();

// Approval mode is the one setting a running session can toggle, so it cannot
// live in environ: spawning a child iterates environ on another thread while
// /yolo would be rewriting it. Children receive it as an explicit override.
enum class ApprovalMode { kAsk, kAuto, kYolo };
const char* ApprovalModeName(ApprovalMode mode);
bool ParseApprovalMode(std::string_view value, ApprovalMode& mode);
ApprovalMode CurrentApprovalMode();
void SetApprovalMode(ApprovalMode mode);
bool ApprovalIsYolo();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_ENV_H_
