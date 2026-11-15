// Copyright 2026 Timon Gentzsch

#include "include/app/self_description.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/agent/agent.h"
#include "include/agent/prompt.h"
#include "include/api/client.h"
#include "include/app/agent_tools.h"
#include "include/app/config_proposal.h"
#include "include/app/present.h"
#include "include/app/uagent_tool.h"
#include "include/app/update.h"
#include "include/cli/cli.h"
#include "include/cli/options.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/sandbox.h"
#include "include/core/strings.h"
#include "include/mcp/register.h"
#include "include/providers/providers.h"
#include "include/tools/browser.h"
#include "include/tools/registry.h"
#include "include/tools/skill.h"
#include "include/tools/tool.h"
#include "include/tools/web_fetch.h"
#include "include/tools/web_search.h"

namespace uagent {
namespace {

struct TopicName {
  SelfTopic topic;
  const char* name;
};

constexpr TopicName kTopics[] = {
    {SelfTopic::kStatus, "status"},     {SelfTopic::kCli, "cli"},
    {SelfTopic::kCommands, "commands"}, {SelfTopic::kConfig, "config"},
    {SelfTopic::kPrompt, "prompt"},     {SelfTopic::kTools, "tools"},
    {SelfTopic::kRoutes, "routes"},     {SelfTopic::kMcp, "mcp"},
};

json DescriptorJson(const ConfigDescriptor& descriptor) {
  json entry = {{"name", descriptor.environment},
                {"key", descriptor.key},
                {"type", ConfigTypeName(descriptor.type)},
                {"default", std::visit([](auto value) { return json(value); },
                                       descriptor.default_value)},
                {"takes_effect", ReloadPolicyName(descriptor.reload)},
                {"sensitivity", SensitivityName(descriptor.sensitivity)},
                {"category", descriptor.category},
                {"description", descriptor.description}};
  if (!descriptor.field.empty()) entry["field"] = descriptor.field;
  if (!descriptor.choices.empty()) entry["choices"] = descriptor.choices;
  if (!descriptor.fallback.empty()) {
    entry[FindConfigDescriptor(descriptor.fallback) ? "follows" : "fallback"] =
        descriptor.fallback;
  }
  entry["label"] = descriptor.label;
  entry["scopes"] = json::array();
  for (const ConfigScopeName& scope : kConfigScopes) {
    if (descriptor.scopes & scope.persisted) {
      entry["scopes"].push_back(scope.source);
    }
  }
  if (!descriptor.purpose.empty()) entry["purpose"] = descriptor.purpose;
  if (descriptor.terminal) entry["terminal"] = true;
  if (descriptor.type == ConfigType::kInt) {
    if (descriptor.minimum != kConfigAnyMin) {
      entry["minimum"] = descriptor.minimum;
    }
    if (descriptor.maximum != kConfigAnyMax) {
      entry["maximum"] = descriptor.maximum;
    }
  }
  return entry;
}

json FlagJson(const FlagSpec& spec) {
  json entry = {{"flag", spec.flag},
                {"usage", FlagInvocation(spec)},
                {"description", spec.help ? spec.help : ""}};
  if (spec.value) entry["value"] = spec.value;
  if (spec.key) entry["setting"] = spec.key;
  if (spec.preset) entry["sets"] = spec.preset;
  return entry;
}

json CommandJson(const SlashCommandSpec& command) {
  std::string usage = command.name;
  if (*command.argument) usage += std::string(" ") + command.argument;
  json aliases = json::array();
  for (const auto& entry : SlashCommandRegistry()) {
    if (entry.id == command.id && !*entry.description) {
      aliases.push_back(entry.name);
    }
  }
  return {{"command", command.name},
          {"argument", command.argument},
          {"aliases", std::move(aliases)},
          {"usage", std::move(usage)},
          {"description", command.description},
          {"alias", !*command.description},
          {"client_only", command.Has(kClientOnly)},
          {"terminal", command.Has(kTerminal)}};
}

}  // namespace

bool ParseSelfTopic(std::string_view name, SelfTopic& topic) {
  for (const TopicName& entry : kTopics) {
    if (name == entry.name) {
      topic = entry.topic;
      return true;
    }
  }
  return false;
}

const char* SelfTopicName(SelfTopic topic) {
  for (const TopicName& entry : kTopics) {
    if (entry.topic == topic) return entry.name;
  }
  return "status";
}

json ConfigSchemaJson() {
  json settings = json::array();
  for (const ConfigDescriptor& descriptor : ConfigRegistry()) {
    settings.push_back(DescriptorJson(descriptor));
  }
  return settings;
}

// `text` as the setting's type; null when empty or unparsable.
json TypedValue(const ConfigDescriptor& descriptor, const std::string& text) {
  int64_t integer = 0;
  double number = 0;
  bool flag = false;
  switch (descriptor.type) {
    case ConfigType::kInt:
      return ParseInt64(text.c_str(), integer)
                 ? json(std::clamp(integer, descriptor.minimum,
                                   descriptor.maximum))
                 : json();
    case ConfigType::kDouble:
      return ParseFiniteDouble(text.c_str(), number) ? json(number) : json();
    case ConfigType::kBool:
      return ParseBool(text, flag) ? json(flag) : json();
    case ConfigType::kString:
      return text.empty() ? json() : json(text);
  }
  return json();
}

json ConfigSettingsJson(const EffectiveConfigSnapshot& configured,
                        std::string_view name) {
  json settings = json::array();
  for (const ConfigDescriptor& descriptor : ConfigRegistry()) {
    if (!name.empty() && descriptor.environment != name &&
        descriptor.key != name) {
      continue;
    }
    json entry = DescriptorJson(descriptor);
    const std::string key(descriptor.environment);
    const bool secret = descriptor.sensitivity != Sensitivity::kPublic;
    const std::string source =
        JsonValue(configured.sources, key.c_str(), "default");
    entry["source"] = source;
    entry["locked"] = source == "environment" || source == "cli";
    for (const auto& [scope, held] : configured.layers) {
      if (auto own = held.find(key);
          own != held.end() && !own->second.empty()) {
        // The providers are shown with their keys hidden; from a variable,
        // they are only said to be set.
        json shown =
            json::parse(SanitizeCompositeValue(own->second), nullptr, false);
        entry["set"][scope] = !secret ? TypedValue(descriptor, own->second)
                              : shown.is_object() ? std::move(shown)
                                                  : json(true);
      }
    }
    auto merged = configured.values.find(key);
    if (!secret) {
      json effective = merged != configured.values.end()
                           ? TypedValue(descriptor, merged->second)
                           : json();
      entry["effective"] =
          effective.is_null() ? entry["default"] : std::move(effective);
    } else if (merged != configured.values.end()) {
      // The providers in effect, so their names can be read; keys hidden.
      json shown =
          json::parse(SanitizeCompositeValue(merged->second), nullptr, false);
      if (shown.is_object()) entry["effective"] = std::move(shown);
    }
    settings.push_back(std::move(entry));
  }
  // An empty setting that follows another takes that one's value.
  for (json& entry : settings) {
    if (!entry.contains("follows") || entry.value("effective", "") != "") {
      continue;
    }
    for (const json& other : settings) {
      if (other["name"] == entry["follows"] && other.contains("effective")) {
        entry["effective"] = other["effective"];
      }
    }
  }
  return settings;
}

json CliSchemaJson() {
  json flags = json::array();
  for (const FlagSpec& spec : FlagRegistry()) {
    if (!spec.help || !*spec.help) continue;  // hidden alias
    flags.push_back(FlagJson(spec));
  }
  return flags;
}

const json& CommandSchemaJson() {
  static const json kCommands = [] {
    json commands = json::array();
    for (const SlashCommandSpec& command : SlashCommandRegistry()) {
      if (!*command.description) continue;
      commands.push_back(CommandJson(command));
    }
    return commands;
  }();
  return kCommands;
}

json DescribeSelf(SelfTopic topic, const std::string& name,
                  const SelfDescriptionInputs& inputs) {
  json out = {{"topic", SelfTopicName(topic)}, {"version", kVersion}};
  switch (topic) {
    case SelfTopic::kStatus: {
      out["model"] = inputs.api.model;
      out["route"] = RouteSelection(inputs.api, {});
      out["base_url"] = RedactedUrl(inputs.api.base_url);
      out["effort"] = inputs.api.reasoning_effort.empty()
                          ? "provider default"
                          : inputs.api.reasoning_effort;
      out["wire_api"] = WireApiName(inputs.api.wire_api);
      out["approval"] = ApprovalModeName(CurrentApprovalMode());
      out["context_window"] = inputs.api.ctx_window;
      out["tools"] = inputs.tools.size();
      out["session_budget"] = inputs.active.session_budget;
      out["max_turn_cost"] = inputs.active.max_turn_cost;
      out["memory"] = inputs.active.memory_enabled;
      out["web_search"] = inputs.active.web_search_backend;
      out["sandbox"] = SandboxDiagnosticJson();
      // A newer release, as About last read it; only a person installs it.
      if (const json update = UpdateState(false);
          JsonValue(update, "available", false)) {
        out["update"] = update["latest"];
      }
      json diagnostics = inputs.config_manager.DiagnosticJson(inputs.active);
      out["restart_required"] = diagnostics["restart_required"];
      break;
    }
    case SelfTopic::kCli:
      out["flags"] = CliSchemaJson();
      break;
    case SelfTopic::kCommands:
      out["commands"] = CommandSchemaJson();
      break;
    case SelfTopic::kConfig: {
      out["settings"] = ConfigSettingsJson(inputs.config_manager.Read(), name);
      out["restart_required"] = inputs.config_manager.DiagnosticJson(
          inputs.active)["restart_required"];
      break;
    }
    case SelfTopic::kRoutes: {
      // Which routes exist, never their credentials. Without it a model
      // guesses, and a wrong guess fails as an authentication error.
      ProviderCatalog catalog = LoadProviderCatalog();
      json models = json::array();
      for (const Route& route : catalog.models) {
        if (!name.empty() && route.route_name != name) continue;
        models.push_back({{"name", route.route_name},
                          {"model", route.model},
                          {"base_url", RedactedUrl(route.base_url)},
                          {"wire_api", WireApiName(route.wire_api)},
                          {"features", FeatureNames(route.features)},
                          {"context", route.ctx_window},
                          {"effort", route.reasoning_effort},
                          {"credential", HasKey(route) ? "set" : "unset"}});
      }
      json providers = json::array();
      for (const Endpoint& provider : catalog.providers) {
        if (!name.empty() && provider.name != name) continue;
        providers.push_back(
            {{"name", provider.name},
             {"base_url", RedactedUrl(provider.base_url)},
             {"wire_api", WireApiName(provider.wire_api)},
             {"features", FeatureNames(provider.features)},
             {"context", provider.context},
             {"credential", HasKey(provider) ? "set" : "unset"}});
      }
      out["active"] = RouteSelection(inputs.api, catalog.providers);
      out["models"] = std::move(models);
      out["providers"] = std::move(providers);
      out["selection"] = "provider/model[:effort]";
      out["efforts"] = kReasoningEfforts;
      out["note"] =
          "A named model route resolves by its own name; any other id is "
          "provider/model. Ids a provider serves are not enumerated here — "
          "only what this build is configured to reach; change providers with "
          "uagent set.";
      break;
    }
    case SelfTopic::kMcp:
      // Not a server's command line or its log: either can carry a
      // credential.
      out["servers"] = json::array();
      for (const json& server : McpStatus(inputs.mcp, inputs.tools)) {
        out["servers"].push_back(
            JsonPick(server, {"name", "scope", "file", "state", "tools"}));
      }
      break;
    case SelfTopic::kPrompt: {
      if (inputs.agent) return inputs.agent->PromptPreview();
      out.update(
          ResolvePrompt(ApplyPromptOverlay(SystemPromptBase(),
                                           PromptOverlay(nullptr), nullptr) +
                        CapabilityPrompt(inputs.tools)));
      out["preview_kind"] =
          "Base prompt without active conversation or repository context.";
      break;
    }
    case SelfTopic::kTools: {
      if (inputs.agent && name.empty()) {
        out.update(inputs.agent->ToolCatalogue());
        break;
      }
      json tools = json::array();
      for (const Tool& tool : inputs.tools) {
        if (!name.empty() && tool.name != name) continue;
        tools.push_back({{"name", tool.name},
                         {"title", ToolTitle(tool)},
                         {"category", ToolCategory(tool)},
                         {"description", tool.description},
                         {"parameters", tool.parameters}});
      }
      out["tools"] = std::move(tools);
      break;
    }
  }
  return out;
}

json PromptSurfaceJson() {
  // Each capability fragment renders against a probe registry naming just its
  // trigger: the surface is a function of the source alone, for the CI diff.
  auto probe = [](const char* name) {
    return MakeTool(
        name, "", json::object(),
        [](const json&, const ToolContext&) { return ToolSuccess(""); });
  };
  json fragments = json::array();
  for (const char* trigger :
       {"activity", "web_search", "web_fetch", "browser"}) {
    std::vector<Tool> probes = {probe(trigger)};
    fragments.push_back(
        {{"trigger", trigger}, {"text", CapabilityPrompt(probes)}});
  }
  std::vector<Tool> research = {probe("web_search"), probe("agent_start")};
  fragments.push_back({{"trigger", "web_search+agent_start"},
                       {"text", CapabilityPrompt(research)}});
  json sections = json::array();
  for (std::string_view section : PromptSections()) {
    sections.push_back(section);
  }
  std::string base = SystemPromptBase();
  return {{"base", base},
          {"base_chars", base.size()},
          {"overlay_sections", std::move(sections)},
          {"capability_fragments", std::move(fragments)}};
}

json ToolSurfaceJson() {
  // Every schema the model can be charged for, conditional tools included.
  // Factories get empty dependencies: the emitted text is the template.
  ActivitySupervisor supervisor;
  Api api;
  UsageAccumulator usage;
  const std::string workspace = CanonicalAccessPath(".");
  std::vector<Tool> tools = BuiltinTools(supervisor, workspace);
  std::vector<std::pair<Tool, const char*>> conditional;
  conditional.emplace_back(
      UagentTool([](SelfTopic, const std::string&) { return json::object(); },
                 [](ConfigProposalScope, const std::vector<ConfigChange>&) {
                   return ConfigProposal{};
                 },
                 [](const std::string&) {}),
      "uagent-config skill open; set and write where a person can approve, "
      "command in a conversation's own session");
  conditional.emplace_back(WebSearchTool(api, usage, {}), "search route");
  conditional.emplace_back(WebFetchTool(api), "always");
  std::vector<Tool> agent_tools;
  AddAgentTools(agent_tools, api, [] { return std::string(); });
  for (Tool& tool : agent_tools) {
    const char* when = tool.name == "agent_start" ? "agent depth" : "always";
    conditional.emplace_back(std::move(tool), when);
  }
  conditional.emplace_back(PresentTool(""), "a session with a client");
  conditional.emplace_back(ScheduleTool([] { return std::string(); }),
                           "a session with a client");
  conditional.emplace_back(
      BrowserTool("", nullptr),
      "a top-level session while the web host runs the browser");
  conditional.emplace_back(SkillTool({}, {}), "skills installed");

  json out = json::array();
  auto emit = [&out](const Tool& tool, const char* when) {
    // ToolDescription, not the raw field: the batching and budget suffixes are
    // part of what the model reads.
    out.push_back({{"name", tool.name},
                   {"description", ToolDescription(tool)},
                   {"parameters", ToolParameters(tool)},
                   {"parallel_safe", tool.parallel_safe},
                   {"when", when}});
  };
  for (const Tool& tool : tools) {
    const char* when = "always";
    if (tool.memory_store) {
      when = "memory enabled";
    }
    emit(tool, when);
  }
  for (const auto& [tool, when] : conditional) emit(tool, when);
  return out;
}

json BuildProvenanceJson() {
  const std::string schema_digest =
      HashHex(JsonDump(json{{"config", ConfigSchemaJson()},
                            {"cli", CliSchemaJson()},
                            {"commands", CommandSchemaJson()}}));
  const json surface = {{"schema", schema_digest},
                        {"tools", HashHex(JsonDump(ToolSurfaceJson()))}};
  json provenance = {
      {"format", 1},
      {"binary_version", kVersion},
      {"prompt_digest", HashHex(JsonDump(PromptSurfaceJson()))},
      {"surface_digest", HashHex(JsonDump(surface))},
  };
  // A deterministic release identity, not a host, path, timestamp or other
  // value that can split equivalent cohorts or disclose local state.
  provenance["build_id"] = HashHex(JsonDump(provenance));
  return provenance;
}

}  // namespace uagent
