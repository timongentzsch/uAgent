// Copyright 2026 Timon Gentzsch

#include "include/app/self_description.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/app/options.h"
#include "include/cli.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/providers.h"
#include "include/tools/tool.h"

namespace uagent {
namespace {

struct TopicName {
  SelfTopic topic;
  const char* name;
};

constexpr TopicName kTopics[] = {
    {SelfTopic::kStatus, "status"},     {SelfTopic::kCli, "cli"},
    {SelfTopic::kCommands, "commands"}, {SelfTopic::kConfig, "config"},
    {SelfTopic::kTools, "tools"},
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
    if (descriptor.minimum != kConfigAnyMin)
      entry["minimum"] = descriptor.minimum;
    if (descriptor.maximum != kConfigAnyMax)
      entry["maximum"] = descriptor.maximum;
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

}  // namespace uagent
