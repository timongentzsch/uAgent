// Copyright 2026 Timon Gentzsch

#include "include/app/self_description.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/prompt.h"
#include "include/api.h"
#include "include/app/config_proposal.h"
#include "include/app/options.h"
#include "include/cli.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/providers.h"
#include "include/tools/configure.h"
#include "include/tools/process.h"
#include "include/tools/registry.h"
#include "include/tools/self_info.h"
#include "include/tools/skill.h"
#include "include/tools/subagent.h"
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
    {SelfTopic::kRoutes, "routes"},
};

json DefaultJson(const ConfigDescriptor& descriptor) {
  if (const int64_t* value = std::get_if<int64_t>(&descriptor.default_value)) {
    return *value;
  }
  if (const double* value = std::get_if<double>(&descriptor.default_value)) {
    return *value;
  }
  if (const bool* value = std::get_if<bool>(&descriptor.default_value)) {
    return *value;
  }
  if (const std::string_view* value =
          std::get_if<std::string_view>(&descriptor.default_value)) {
    return *value;
  }
  return nullptr;  // derived from another setting at read time
}

json DescriptorJson(const ConfigDescriptor& descriptor) {
  json entry = {{"name", descriptor.environment},
                {"type", ConfigTypeName(descriptor.type)},
                {"default", DefaultJson(descriptor)},
                {"takes_effect", ReloadPolicyName(descriptor.reload)},
                {"sensitivity", SensitivityName(descriptor.sensitivity)},
                {"category", descriptor.category},
                {"description", descriptor.description}};
  if (!descriptor.field.empty()) entry["field"] = descriptor.field;
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

std::string FlagInvocation(const FlagSpec& spec) {
  std::string invocation(spec.flag);
  if (!spec.value) return invocation;
  return spec.optional_value ? invocation + "[=" + spec.value + "]"
                             : invocation + " " + spec.value;
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
  return {{"command", command.name},
          {"usage", std::move(usage)},
          {"description", command.description},
          {"alias", !*command.description}};
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

json CliSchemaJson() {
  json flags = json::array();
  for (const FlagSpec& spec : FlagRegistry()) {
    if (!spec.help || !*spec.help) continue;  // hidden alias
    flags.push_back(FlagJson(spec));
  }
  return flags;
}

json CommandSchemaJson() {
  json commands = json::array();
  for (const SlashCommandSpec& command : SlashCommandRegistry()) {
    if (!*command.description) continue;
    commands.push_back(CommandJson(command));
  }
  return commands;
}

json DescribeSelf(SelfTopic topic, const std::string& name,
                  const SelfDescriptionInputs& inputs) {
  json out = {{"topic", SelfTopicName(topic)}, {"version", kVersion}};
  switch (topic) {
    case SelfTopic::kStatus: {
      out["model"] = inputs.api.RequestModel();
      out["route"] = RouteSelection(inputs.api, {});
      out["base_url"] = RedactedUrl(inputs.api.base_url);
      out["effort"] = inputs.api.reasoning_effort.empty()
                          ? "provider default"
                          : inputs.api.reasoning_effort;
      out["wire_api"] = WireApiName(inputs.api.capabilities.wire_api);
      out["approval"] = inputs.yolo ? "yolo" : "ask";
      out["context_window"] = inputs.api.ctx_window;
      out["tools"] = inputs.tools.size();
      out["session_budget"] = inputs.active.session_budget;
      out["max_turn_cost"] = inputs.active.max_turn_cost;
      out["memory"] = inputs.active.memory_enabled;
      out["web_search"] = inputs.active.web_search_backend;
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
      json diagnostics = inputs.config_manager.DiagnosticJson(inputs.active);
      const json& sources = diagnostics["sources"];
      const json& active = diagnostics["active"];
      json settings = json::array();
      for (const ConfigDescriptor& descriptor : ConfigRegistry()) {
        if (!name.empty() && descriptor.environment != name) continue;
        json entry = DescriptorJson(descriptor);
        entry["source"] =
            JsonValue(sources, std::string(descriptor.environment).c_str(),
                      std::string("default"));
        if (!descriptor.field.empty() &&
            active.contains(std::string(descriptor.field))) {
          entry["active"] = active[std::string(descriptor.field)];
        }
        settings.push_back(std::move(entry));
      }
      out["settings"] = std::move(settings);
      out["restart_required"] = diagnostics["restart_required"];
      break;
    }
    case SelfTopic::kRoutes: {
      // Which routes exist, never their credentials. A model that cannot read
      // this guesses a selection, and a guess that resolves to the wrong
      // endpoint fails as an authentication error rather than as a typo.
      ProviderCatalog catalog = SessionProviderCatalog();
      json models = json::array();
      for (const ModelRoute& route : catalog.models) {
        if (!name.empty() && route.name != name) continue;
        models.push_back(
            {{"name", route.name},
             {"model", route.model},
             {"base_url", RedactedUrl(route.base_url)},
             {"protocol", ProviderProtocolName(route.protocol)},
             {"wire_api", WireApiName(route.wire_api)},
             {"context", route.context},
             {"effort", route.effort},
             {"hosted_web_search", route.hosted_web_search},
             {"credential", route.api_key.empty() ? "unset" : "set"}});
      }
      json providers = json::array();
      for (const NamedProvider& provider : catalog.providers) {
        if (!name.empty() && provider.name != name) continue;
        providers.push_back(
            {{"name", provider.name},
             {"base_url", RedactedUrl(provider.base_url)},
             {"protocol", ProviderProtocolName(provider.protocol)},
             {"wire_api", WireApiName(provider.wire_api)},
             {"context", provider.context},
             {"hosted_web_search", provider.hosted_web_search},
             {"credential", provider.api_key.empty() ? "unset" : "set"}});
      }
      out["active"] = RouteSelection(inputs.api, catalog.providers);
      out["models"] = std::move(models);
      out["providers"] = std::move(providers);
      out["selection"] = "[provider/]model[:variant][:effort]";
      out["efforts"] = json::array();
      for (const char* effort : kReasoningEfforts) {
        out["efforts"].push_back(effort);
      }
      out["note"] =
          "A named model route resolves by its own name; any other id resolves "
          "against a provider scope. Ids a provider serves are not enumerated "
          "here — only what this build is configured to reach.";
      break;
    }
    case SelfTopic::kPrompt: {
      // Identity, not a copy: message zero is already in the model's context,
      // so what cannot be seen from there is which base, which conditional
      // sections and which experiment produced it.
      std::string base = SystemPromptBase();
      json sections = json::array();
      for (std::string_view section : PromptSections()) {
        sections.push_back(section);
      }
      std::string capabilities = CapabilityPrompt(inputs.tools);
      json triggers = json::array();
      for (const char* trigger :
           {"activity", "web_search", "web_fetch", "adapt_system"}) {
        if (FindTool(inputs.tools, trigger)) triggers.push_back(trigger);
      }
      out["base"] = {{"chars", base.size()},
                     {"digest", HashHex(base).substr(0, 12)},
                     {"sections", std::move(sections)}};
      out["capabilities"] = {{"chars", capabilities.size()},
                             {"triggers", std::move(triggers)}};
      out["host_capabilities"] = {
          {"chars", HostCapabilityPrompt(inputs.tools).size()}};
      std::string digest;
      json overlay = PromptOverlay(&digest);
      std::vector<std::string> applied;
      ApplyPromptOverlay(base, overlay, &applied);
      out["overlay"] = {{"path", PromptOverlayPath()},
                        {"digest", digest},
                        {"applied", applied}};
      out["note"] =
          "Project instructions, the memory index and any mutable directive "
          "are appended per session; --debug records the exact bytes sent.";
      break;
    }
    case SelfTopic::kTools: {
      json tools = json::array();
      for (const Tool& tool : inputs.tools) {
        if (!name.empty() && tool.name != name) continue;
        tools.push_back({{"name", tool.name},
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
  // Capability fragments are conditional on a registered tool, so each one is
  // rendered against a probe registry naming just its trigger. Nothing here
  // reads the environment or a live session: the emitted surface has to be a
  // function of the source alone for the CI diff to mean anything.
  auto probe = [](const char* name) {
    return MakeTool(
        name, "", json::object(),
        [](const json&, const ToolContext&) { return ToolSuccess(""); });
  };
  json fragments = json::array();
  for (const char* trigger :
       {"activity", "web_search", "web_fetch", "adapt_system"}) {
    std::vector<Tool> probes = {probe(trigger)};
    fragments.push_back(
        {{"trigger", trigger}, {"text", CapabilityPrompt(probes)}});
  }
  std::vector<Tool> research = {probe("web_search"), probe("subagent")};
  fragments.push_back({{"trigger", "web_search+subagent"},
                       {"text", CapabilityPrompt(research)}});
  json sections = json::array();
  for (std::string_view section : PromptSections()) {
    sections.push_back(section);
  }
  std::string base = SystemPromptBase();
  return {{"base", base},
          {"base_chars", base.size()},
          {"overlay_sections", std::move(sections)},
          {"capability_fragments", std::move(fragments)},
          {"text_protocol_preamble", TextProtocolPrompt({})}};
}

json ToolSurfaceJson() {
  // Every schema the model can be charged for, not just the built-in ones.
  // The conditionally registered tools are the largest schemas in the surface,
  // so leaving them out left most of the token cost outside the drift gate.
  // Each factory is constructed with empty dependencies: the emitted text is
  // the template, and the route- and catalogue-dependent parts a live session
  // splices in are reported by `/context` instead.
  ProcessSupervisor supervisor;
  AdaptiveSystemState adaptive_system;
  Api api;
  UsageAccumulator usage;
  const std::string workspace = CanonicalAccessPath(".");
  std::vector<Tool> tools = BuiltinTools(
      supervisor, workspace, /*inline_images=*/false, &adaptive_system);
  // The image tool exists only where the terminal can draw, so it is collected
  // from a second registry rather than left out of the gate entirely.
  for (Tool& tool : BuiltinTools(supervisor, workspace, /*inline_images=*/true,
                                 &adaptive_system)) {
    if (!FindTool(tools, tool.name)) tools.push_back(std::move(tool));
  }
  std::vector<std::pair<Tool, const char*>> conditional;
  conditional.emplace_back(SelfInfoTool([](SelfTopic, const std::string&) {
                             return json::object();
                           }),
                           "always");
  conditional.emplace_back(
      ConfigureTool(
          [](ConfigProposalScope, const std::vector<ConfigChange>&) {
            return ConfigProposal{};
          },
          std::make_shared<ConfigProposalStore>()),
      "interactive terminal");
  conditional.emplace_back(WebSearchTool(api, usage, {}), "search route");
  conditional.emplace_back(WebFetchTool(api), "always");
  conditional.emplace_back(
      SubagentTool(api, supervisor, {}, {}, /*debug=*/false),
      "delegation depth");
  conditional.emplace_back(SkillTool({}, {}), "skills installed");

  json out = json::array();
  auto emit = [&out](const Tool& tool, const char* when) {
    // ToolDescription, not the raw field: the batching and budget suffixes are
    // part of what the model reads.
    out.push_back({{"name", tool.name},
                   {"description", ToolDescription(tool)},
                   {"parameters", ToolParameters(tool)},
                   {"lean", tool.available_in_lean},
                   {"parallel_safe", tool.parallel_safe},
                   {"when", when}});
  };
  for (const Tool& tool : tools) {
    const char* when = "always";
    if (tool.visibility == Tool::Visibility::kDetachedTerminal) {
      when = "detached activity";
    } else if (tool.memory_store) {
      when = "memory enabled";
    } else if (tool.name == "adapt_system") {
      when = "adapt_system enabled";
    } else if (tool.replay_image) {
      when = "terminal images";
    }
    emit(tool, when);
  }
  for (const auto& [tool, when] : conditional) emit(tool, when);
  return out;
}

}  // namespace uagent
