// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SELF_DESCRIPTION_H_
#define UAGENT_INCLUDE_APP_SELF_DESCRIPTION_H_
// Read-only answers about this binary, assembled on demand from the
// registries: nothing is injected into the prompt.

#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {
class Agent;

class Api;
class ConfigManager;
class McpRuntime;
struct RuntimeConfig;
struct EffectiveConfigSnapshot;
struct Tool;

enum class SelfTopic {
  kStatus,
  kCli,
  kCommands,
  kConfig,
  kTools,
  kPrompt,
  kRoutes,
  kMcp,
};

// The live state a description needs; held by reference for one call only.
struct SelfDescriptionInputs {
  const ConfigManager& config_manager;
  const RuntimeConfig& active;
  const Api& api;
  const std::vector<Tool>& tools;
  const McpRuntime& mcp;
  Agent* agent = nullptr;
};

bool ParseSelfTopic(std::string_view name, SelfTopic& topic);
const char* SelfTopicName(SelfTopic topic);

// `name` narrows config/cli/commands to one entry; empty returns the topic.
// Secret values are reported as set/unset and never returned verbatim.
json DescribeSelf(SelfTopic topic, const std::string& name,
                  const SelfDescriptionInputs& inputs);

json ConfigSchemaJson();
// Per setting (one when `name` is set): its descriptor, `scopes`, `set`,
// `effective`, `source` and `locked`. A secret reports only that it is set.
json ConfigSettingsJson(const EffectiveConfigSnapshot& configured,
                        std::string_view name);
json CliSchemaJson();
// Every listed command; `terminal` marks those the web does not offer.
const json& CommandSchemaJson();

// The base prompt with every capability fragment and the built-in tool
// schemas: both deterministic, so their digests identify a release.
json PromptSurfaceJson();
json ToolSurfaceJson();

// Digests of the surfaces above plus the binary version.
json BuildProvenanceJson();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_SELF_DESCRIPTION_H_
