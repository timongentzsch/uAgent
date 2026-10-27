// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SELF_DESCRIPTION_H_
#define UAGENT_INCLUDE_APP_SELF_DESCRIPTION_H_
// Read-only answers about this binary: version, flags, commands, configuration
// schema, effective values with provenance, and the live tool surface. Every
// topic is assembled on demand from the authoritative registries, so nothing is
// injected into the prompt and startup pays no cost.

#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {
class Agent;

class Api;
class ConfigManager;
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
};

// The live state a description needs; held by reference for one call only.
struct SelfDescriptionInputs {
  const ConfigManager& config_manager;
  const RuntimeConfig& active;
  const Api& api;
  const std::vector<Tool>& tools;
  Agent* agent = nullptr;
};

bool ParseSelfTopic(std::string_view name, SelfTopic& topic);
const char* SelfTopicName(SelfTopic topic);

// `name` narrows config/cli/commands to one entry; empty returns the topic.
// Secret values are reported as set/unset and never returned verbatim.
json DescribeSelf(SelfTopic topic, const std::string& name,
                  const SelfDescriptionInputs& inputs);

// The configuration schema alone.
json ConfigSchemaJson();
// One entry of facts per setting (one setting when `name` is set): its
// descriptor, `scopes` (where it may be saved), `set` (what each scope
// holds, by scope), `effective` (the value that applies), `source` (the
// scope it comes from), `locked`. A secret reports only that it is set.
json ConfigSettingsJson(const EffectiveConfigSnapshot& configured,
                        std::string_view name);
json CliSchemaJson();
// Every listed command; `terminal` marks those the web does not offer.
const json& CommandSchemaJson();

// The model-facing surface: the base prompt with its sections and every
// capability fragment, and the built-in tool schemas as the model receives
// them. Both are deterministic — no environment, no live session — so their
// digests identify a release. Per-session additions (host capabilities, runtime
// context, the mutable directive) are recorded by `--debug` instead.
json PromptSurfaceJson();
json ToolSurfaceJson();

// Digests of the surfaces above plus the binary version, logged at startup so
// two sessions can be told apart by what they were built to say and do.
json BuildProvenanceJson();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_SELF_DESCRIPTION_H_
