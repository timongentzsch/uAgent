// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PROMPT_H_
#define UAGENT_INCLUDE_AGENT_PROMPT_H_
// System prompt authoring: the base, the capability sections that depend on
// the registered tools, the overlay, and the runtime context line.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

const char* SystemPromptBase();

// The base sections an overlay may replace, in prompt order.
std::vector<std::string_view> PromptSections();
std::string_view PromptSectionText(std::string_view name);

// Why `overlay` cannot be saved; empty when it can.
std::string PromptOverlayIssue(const json& overlay);

// Changes base-prompt text only; `applied` collects the sections it changed.
std::string ApplyPromptOverlay(std::string prompt, const json& overlay,
                               std::vector<std::string>* applied);

// The saved or the experiment's overlay; {} when missing or malformed.
json PromptOverlay(std::string* digest);

// Workflow rules for the registered tools only, kept out of the cacheable base.
std::string CapabilityPrompt(const std::vector<Tool>& tools,
                             const ToolSelection* selection = nullptr);

// {effective, sources, digest, bytes}, or {error} past 64 KiB.
json ResolvePrompt(const std::string& base,
                   const json& context = json::array());

// `sandbox` is SandboxWorkspaceRefusal(): set when commands cannot write here.
std::string EnvironmentContext(const std::string& date, const std::string& cwd,
                               const std::string& approval,
                               const std::string& sandbox = "");

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PROMPT_H_
