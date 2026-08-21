// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {
namespace {

uint32_t CapabilityNamed(const std::string& name) {
  if (name == "inspect") return Capability(ToolCapability::kInspect);
  if (name == "execute") return Capability(ToolCapability::kExecute);
  if (name == "mutate") return Capability(ToolCapability::kMutate);
  if (name == "delegate") return Capability(ToolCapability::kDelegate);
  if (name == "external") return Capability(ToolCapability::kExternal);
  return 0;
}

bool ExactRunAllowed(const ToolPolicy& policy, const json& arguments) {
  std::string command = JsonValue(arguments, "command", "");
  return std::find(policy.run_allowlist.begin(), policy.run_allowlist.end(),
                   command) != policy.run_allowlist.end();
}

bool JsonSchemaTypeMatches(const json& value, std::string_view type) {
  return (type == "string" && value.is_string()) ||
         (type == "integer" && value.is_number_integer()) ||
         (type == "number" && value.is_number()) ||
         (type == "boolean" && value.is_boolean()) ||
         (type == "object" && value.is_object()) ||
         (type == "array" && value.is_array()) ||
         (type == "null" && value.is_null());
}

std::string JsonSchemaTypeLabel(const json& type) {
  if (type.is_string()) return type.get<std::string>();
  if (!type.is_array()) return "the expected type";
  std::string label;
  for (const json& item : type) {
    if (!item.is_string()) continue;
    if (!label.empty()) label += " or ";
    label += item.get_ref<const std::string&>();
  }
  return label.empty() ? "the expected type" : label;
}

std::optional<size_t> JsonSchemaSize(const json& schema,
                                     std::string_view name) {
  auto value = schema.find(name);
  if (value == schema.end()) return std::nullopt;
  if (value->is_number_unsigned()) {
    uint64_t size = value->get<uint64_t>();
    return size > std::numeric_limits<size_t>::max()
               ? std::nullopt
               : std::optional<size_t>(static_cast<size_t>(size));
  }
  if (!value->is_number_integer()) return std::nullopt;
  int64_t size = value->get<int64_t>();
  return size < 0 ? std::nullopt
                  : std::optional<size_t>(static_cast<size_t>(size));
}

std::string InvalidSchemaValue(const json& schema, const json& value,
                               const std::string& path, bool root = false);

std::string InvalidArrayValue(const json& schema, const json& value,
                              const std::string& path) {
  if (std::optional<size_t> minimum = JsonSchemaSize(schema, "minItems");
      minimum && value.size() < *minimum) {
    return "`" + path + "` has too few items";
  }
  if (std::optional<size_t> maximum = JsonSchemaSize(schema, "maxItems");
      maximum && value.size() > *maximum) {
    return "`" + path + "` has too many items";
  }
  auto items = schema.find("items");
  if (items == schema.end() || !items->is_object()) return "";
  for (size_t index = 0; index < value.size(); ++index) {
    std::string invalid = InvalidSchemaValue(
        *items, value[index], path + "[" + std::to_string(index) + "]");
    if (!invalid.empty()) return invalid;
  }
  return "";
}

std::string InvalidObjectValue(const json& schema, const json& value,
                               const std::string& path, bool root) {
  auto required = schema.find("required");
  if (required != schema.end() && required->is_array()) {
    for (const json& item : *required) {
      if (!item.is_string()) continue;
      const std::string& name = item.get_ref<const std::string&>();
      if (value.contains(name)) continue;
      std::string child = root || path.empty() ? name : path + "." + name;
      return "`" + child + "` is required";
    }
  }

  auto found = schema.find("properties");
  const json properties =
      found != schema.end() && found->is_object() ? *found : json::object();
  if (!JsonValue(schema, "additionalProperties", true)) {
    for (const auto& [name, child] : value.items()) {
      (void)child;
      if (properties.contains(name)) continue;
      return "unknown argument `" + (root ? name : path + "." + name) + "`";
    }
  }
  for (const auto& [name, child_schema] : properties.items()) {
    if (!value.contains(name)) continue;
    std::string child = root || path.empty() ? name : path + "." + name;
    std::string invalid = InvalidSchemaValue(child_schema, value[name], child);
    if (!invalid.empty()) return invalid;
  }
  return "";
}

bool JsonSchemaTypesMatch(const json& value, const json& types) {
  if (types.is_string()) {
    return JsonSchemaTypeMatches(value, types.get_ref<const std::string&>());
  }
  if (!types.is_array()) return false;
  return std::any_of(types.begin(), types.end(), [&](const json& type) {
    return type.is_string() &&
           JsonSchemaTypeMatches(value, type.get_ref<const std::string&>());
  });
}

std::string InvalidSchemaValue(const json& schema, const json& value,
                               const std::string& path, bool root) {
  if (!schema.is_object()) return "";
  auto types = schema.find("type");
  if (types != schema.end() && !JsonSchemaTypesMatch(value, *types)) {
    return "`" + path + "` must be " + JsonSchemaTypeLabel(*types);
  }
  auto allowed = schema.find("enum");
  if (allowed != schema.end() && allowed->is_array() &&
      std::find(allowed->begin(), allowed->end(), value) == allowed->end()) {
    return "`" + path + "` is not an allowed value";
  }

  if (value.is_number()) {
    const double number = value.get<double>();
    auto minimum = schema.find("minimum");
    if (minimum != schema.end() && minimum->is_number() &&
        number < minimum->get<double>()) {
      return "`" + path + "` is below its minimum";
    }
    auto maximum = schema.find("maximum");
    if (maximum != schema.end() && maximum->is_number() &&
        number > maximum->get<double>()) {
      return "`" + path + "` is above its maximum";
    }
  }
  if (value.is_string()) {
    const size_t size = value.get_ref<const std::string&>().size();
    if (std::optional<size_t> minimum = JsonSchemaSize(schema, "minLength");
        minimum && size < *minimum) {
      return "`" + path + "` is shorter than its minimum length";
    }
    if (std::optional<size_t> maximum = JsonSchemaSize(schema, "maxLength");
        maximum && size > *maximum) {
      return "`" + path + "` exceeds its maximum length";
    }
  }
  if (value.is_array()) return InvalidArrayValue(schema, value, path);
  if (value.is_object()) return InvalidObjectValue(schema, value, path, root);
  return "";
}

void ReadStringArray(const char* name, std::vector<std::string>& values,
                     std::string& error) {
  std::string configured = EnvStr(name);
  if (configured.empty()) return;
  json parsed = json::parse(configured, nullptr, false);
  if (!parsed.is_array() ||
      !std::all_of(parsed.begin(), parsed.end(),
                   [](const json& value) { return value.is_string(); })) {
    error = std::string(name) + " must be a JSON string array";
    return;
  }
  for (const json& value : parsed) {
    values.push_back(value.get<std::string>());
  }
}

}  // namespace

void ClampToolArguments(const Tool& tool, json& args) {
  if (tool.clamped_arguments.empty() || !args.is_object()) return;
  const auto properties = tool.parameters.find("properties");
  if (properties == tool.parameters.end() || !properties->is_object()) return;
  for (const std::string& name : tool.clamped_arguments) {
    const auto value = args.find(name);
    const auto schema = properties->find(name);
    if (value == args.end() || !value->is_number() ||
        schema == properties->end() || !schema->is_object()) {
      continue;
    }
    const double given = value->get<double>();
    double bounded = given;
    const auto minimum = schema->find("minimum");
    if (minimum != schema->end() && minimum->is_number()) {
      bounded = std::max(bounded, minimum->get<double>());
    }
    const auto maximum = schema->find("maximum");
    if (maximum != schema->end() && maximum->is_number()) {
      bounded = std::min(bounded, maximum->get<double>());
    }
    if (bounded == given) continue;
    // A fractional value for an integer property is still a type error, and
    // clamping must not hide it.
    if (value->is_number_integer()) {
      *value = static_cast<int64_t>(bounded);
    } else {
      *value = bounded;
    }
  }
}

std::string InvalidToolArgument(const Tool& tool, const json& args) {
  return InvalidSchemaValue(ToolParameters(tool), args, tool.name,
                            /*root=*/true);
}

std::string StableArgumentError(
    const Tool& tool, const json& args,
    std::unordered_map<std::string, std::string>& values) {
  if (tool.stable_argument.empty()) return "";
  auto value = args.find(tool.stable_argument);
  if (value == args.end() || !value->is_string()) return "";
  std::string key = tool.name + "\n" + tool.stable_argument;
  auto [found, inserted] = values.emplace(key, value->get<std::string>());
  if (inserted || found->second == value->get_ref<const std::string&>()) {
    return "";
  }
  return "error: `" + tool.stable_argument + "` must remain `" + found->second +
         "` for this turn; reuse that artifact";
}

ToolPolicy ToolPolicyFromEnvironment() {
  ToolPolicy policy;
  std::string configured = Trim(EnvStr("UAGENT_TOOL_CAPABILITIES"));
  if (!configured.empty()) {
    policy.allowed = 0;
    for (const std::string& entry : SplitPathList(configured, ',')) {
      std::string name = Trim(entry);
      uint32_t capability = CapabilityNamed(name);
      if (!capability) {
        policy.error = "unknown tool capability: " + name;
        policy.allowed = 0;
        break;
      }
      policy.allowed |= capability;
    }
  }

  ReadStringArray("UAGENT_TOOL_ALLOWLIST", policy.tool_allowlist, policy.error);
  ReadStringArray("UAGENT_TOOL_RUN_ALLOWLIST", policy.run_allowlist,
                  policy.error);
  return policy;
}

void ApplyToolPolicy(std::vector<Tool>& tools, const ToolPolicy& policy) {
  if (!policy.error.empty()) {
    tools.clear();
    return;
  }
  std::erase_if(tools, [&](Tool& tool) {
    if (!policy.tool_allowlist.empty() &&
        std::find(policy.tool_allowlist.begin(), policy.tool_allowlist.end(),
                  tool.name) == policy.tool_allowlist.end()) {
      return true;
    }
    bool allowed = (tool.capabilities & ~policy.allowed) == 0;
    bool allowlisted_run = tool.command_policy && !policy.run_allowlist.empty();
    if (!allowed && !allowlisted_run) return true;
    if (!tool.command_policy || policy.run_allowlist.empty()) return false;

    tool.validate = [policy](const json& args) {
      if (!ExactRunAllowed(policy, args)) {
        return std::string("error: command is not allowed by tool policy");
      }
      if (JsonValue(args, "detach", false) ||
          JsonValue(args, "shell", "bash") != "bash") {
        return std::string(
            "error: evaluator-authorized commands use foreground bash");
      }
      // The exact allowlist is stronger authority than the general shell
      // heuristic (which normally redirects Python to scratch).
      return std::string();
    };
    tool.description +=
        " Only an evaluator-authorized exact command is allowed.";
    return false;
  });
}

}  // namespace uagent
