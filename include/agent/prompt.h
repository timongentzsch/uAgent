// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PROMPT_H_
#define UAGENT_INCLUDE_AGENT_PROMPT_H_
// System prompt authoring: the immutable base, the capability sections that
// depend on which tools are registered, the experiment overlay, and the
// runtime context line. The text itself lives in prompt.cc, so rewording the
// prompt rebuilds one translation unit instead of every consumer of the
// tool-call protocol.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// The immutable base every session starts from; the text and the reasoning
// behind its shape live in prompt.cc.
const char* SystemPromptBase();

// The base sections an overlay may replace, in prompt order.
std::vector<std::string_view> PromptSections();

// Replace or extend base-prompt sections from a declarative overlay so a
// prompt variant can be measured without rebuilding the binary. This changes
// prompt text only: tools, approvals, capabilities and limits stay host-owned,
// and the sections below the base are not reachable. `applied` collects the
// section names an overlay actually changed.
std::string ApplyPromptOverlay(std::string prompt, const json& overlay,
                               std::vector<std::string>* applied);

// The overlay named by UAGENT_PROMPT_OVERLAY, or an empty object when unset,
// unreadable or malformed: an experiment must not be able to break a session.
// `digest` receives a short content hash when a file was read.
json PromptOverlay(std::string* digest);

// Optional workflow rules, kept out of the cacheable base unless the matching
// tools are actually registered. Tool schemas still own argument-level detail.
std::string CapabilityPrompt(const std::vector<Tool>& tools);

// Host facts the model may not infer from its own claims: which capabilities
// the registry actually offers, and whether mutations need consent.
std::string HostCapabilityPrompt(const std::vector<Tool>& tools);

std::string EnvironmentContext(const std::string& date, const std::string& cwd,
                               int64_t terminal_columns = 0);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PROMPT_H_
