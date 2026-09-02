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

std::optional<ToolArgumentIssue> InvalidSchemaValue(const json& schema,
                                                    const json& value,
                                                    const std::string& path,
                                                    bool root = false);

std::optional<ToolArgumentIssue> InvalidArrayValue(const json& schema,
                                                   const json& value,
                                                   const std::string& path) {
  if (std::optional<size_t> minimum = JsonSchemaSize(schema, "minItems");
      minimum && value.size() < *minimum) {
    return ArgumentIssue("schema.min_items",
                         "`" + path + "` has " + std::to_string(value.size()) +
                             " items, below its minimum " +
                             std::to_string(*minimum),
                         path);
  }
  if (std::optional<size_t> maximum = JsonSchemaSize(schema, "maxItems");
      maximum && value.size() > *maximum) {
    return ArgumentIssue("schema.max_items",
                         "`" + path + "` has " + std::to_string(value.size()) +
                             " items, above its maximum " +
                             std::to_string(*maximum),
                         path);
  }
  auto items = schema.find("items");
  if (items == schema.end() || !items->is_object()) return std::nullopt;
  for (size_t index = 0; index < value.size(); ++index) {
    auto invalid = InvalidSchemaValue(*items, value[index],
                                      path + "[" + std::to_string(index) + "]");
    if (invalid) return invalid;
  }
  return std::nullopt;
}

std::optional<ToolArgumentIssue> InvalidObjectValue(const json& schema,
                                                    const json& value,
                                                    const std::string& path,
                                                    bool root) {
  auto required = schema.find("required");
  if (required != schema.end() && required->is_array()) {
    for (const json& item : *required) {
      if (!item.is_string()) continue;
      const std::string& name = item.get_ref<const std::string&>();
      if (value.contains(name)) continue;
      std::string child = root || path.empty() ? name : path + "." + name;
      return ArgumentIssue("schema.required", "`" + child + "` is required",
                           child);
    }
  }

  auto found = schema.find("properties");
  const json properties =
      found != schema.end() && found->is_object() ? *found : json::object();
  if (!JsonValue(schema, "additionalProperties", true)) {
    for (const auto& [name, child] : value.items()) {
      (void)child;
      if (properties.contains(name)) continue;
      std::string field = root ? name : path + "." + name;
      return ArgumentIssue("schema.additional_property",
                           "unknown argument `" + field + "`", field);
    }
  }
  for (const auto& [name, child_schema] : properties.items()) {
    if (!value.contains(name)) continue;
    std::string child = root || path.empty() ? name : path + "." + name;
    auto invalid = InvalidSchemaValue(child_schema, value[name], child);
    if (invalid) return invalid;
  }
  return std::nullopt;
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

std::optional<ToolArgumentIssue> InvalidSchemaValue(const json& schema,
                                                    const json& value,
                                                    const std::string& path,
                                                    bool root) {
  if (!schema.is_object()) return std::nullopt;
  auto types = schema.find("type");
  if (types != schema.end() && !JsonSchemaTypesMatch(value, *types)) {
    return ArgumentIssue(
        "schema.type", "`" + path + "` must be " + JsonSchemaTypeLabel(*types),
        path);
  }
  auto allowed = schema.find("enum");
  if (allowed != schema.end() && allowed->is_array() &&
      std::find(allowed->begin(), allowed->end(), value) == allowed->end()) {
    return ArgumentIssue("schema.enum",
                         "`" + path + "` is not an allowed value", path);
  }

  if (value.is_number()) {
    const double number = value.get<double>();
    // A rejected bound is only actionable if the caller can see both numbers:
    // the value it sent and the limit it crossed. Without them the next
    // attempt is a guess, and the guess is what the history is full of.
    auto minimum = schema.find("minimum");
    if (minimum != schema.end() && minimum->is_number() &&
        number < minimum->get<double>()) {
      return ArgumentIssue("schema.minimum",
                           "`" + path + "` is " + JsonDump(value) +
                               ", below its minimum " + JsonDump(*minimum),
                           path);
    }
    auto maximum = schema.find("maximum");
    if (maximum != schema.end() && maximum->is_number() &&
        number > maximum->get<double>()) {
      return ArgumentIssue("schema.maximum",
                           "`" + path + "` is " + JsonDump(value) +
                               ", above its maximum " + JsonDump(*maximum),
                           path);
    }
  }
  if (value.is_string()) {
    const size_t size = value.get_ref<const std::string&>().size();
    if (std::optional<size_t> minimum = JsonSchemaSize(schema, "minLength");
        minimum && size < *minimum) {
      return ArgumentIssue("schema.min_length",
                           "`" + path + "` is " + std::to_string(size) +
                               " characters, below its minimum length " +
                               std::to_string(*minimum),
                           path);
    }
    if (std::optional<size_t> maximum = JsonSchemaSize(schema, "maxLength");
        maximum && size > *maximum) {
      return ArgumentIssue("schema.max_length",
                           "`" + path + "` is " + std::to_string(size) +
                               " characters, above its maximum length " +
                               std::to_string(*maximum),
                           path);
    }
  }
  if (value.is_array()) return InvalidArrayValue(schema, value, path);
  if (value.is_object()) return InvalidObjectValue(schema, value, path, root);
  return std::nullopt;
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

namespace {

// Resolve a clamp target, which may name a field one object deep as `a.b`.
// Grouping several pacing hints under one object property is how a tool keeps
// its schema small; they are still pacing hints, so they still clamp. One
// level is deliberate — nothing needs two, and a general path resolver here
// would be machinery without a caller.
bool ResolveClampTarget(json& args, const json& properties,
                        const std::string& name, json*& value,
                        const json*& schema) {
  const size_t dot = name.find('.');
  json* container = &args;
  const json* declared = &properties;
  std::string leaf = name;
  if (dot != std::string::npos) {
    const std::string outer = name.substr(0, dot);
    leaf = name.substr(dot + 1);
    const auto nested = args.find(outer);
    const json* bounds = JsonObject(properties, outer.c_str());
    if (nested == args.end() || !nested->is_object() || bounds == nullptr) {
      return false;
    }
    container = &*nested;
    declared = JsonObject(*bounds, "properties");
    if (declared == nullptr) return false;
  }
  const auto found = container->find(leaf);
  const auto bound = declared->find(leaf);
  if (found == container->end() || !found->is_number() ||
      bound == declared->end() || !bound->is_object()) {
    return false;
  }
  value = &*found;
  schema = &*bound;
  return true;
}

}  // namespace

void ClampToolArguments(const Tool& tool, json& args,
                        std::vector<std::string>* clamped) {
  if (tool.clamped_arguments.empty() || !args.is_object()) return;
  const json* properties = JsonObject(tool.parameters, "properties");
  if (properties == nullptr) return;
  for (const std::string& name : tool.clamped_arguments) {
    json* value = nullptr;
    const json* schema = nullptr;
    if (!ResolveClampTarget(args, *properties, name, value, schema)) continue;
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
    const std::string requested = JsonDump(*value);
    // A fractional value for an integer property is still a type error, and
    // clamping must not hide it.
    if (value->is_number_integer()) {
      *value = static_cast<int64_t>(bounded);
    } else {
      *value = bounded;
    }
    if (clamped) {
      clamped->push_back(name + " to " + JsonDump(*value) + " of " + requested +
                         " requested");
    }
  }
}

void CanonicalizeToolArguments(const Tool& tool, json& args,
                               std::vector<std::string>* clamped) {
  if (!args.is_object()) return;
  if (tool.canonicalize) tool.canonicalize(args);
  ClampToolArguments(tool, args, clamped);
}

std::optional<ToolArgumentIssue> FindToolArgumentIssue(const Tool& tool,
                                                       const json& args) {
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

  ReadStringArray("UAGENT_INTERNAL_TOOL_ALLOWLIST", policy.tool_allowlist,
                  policy.error);
  ReadStringArray("UAGENT_INTERNAL_TOOL_RUN_ALLOWLIST", policy.run_allowlist,
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

    tool.validate =
        [policy](const json& args) -> std::optional<ToolArgumentIssue> {
      if (!ExactRunAllowed(policy, args)) {
        return ArgumentIssue("policy.command",
                             "command is not allowed by tool policy",
                             "command");
      }
      if (JsonValue(args, "detach", false) ||
          JsonValue(args, "shell", "bash") != "bash") {
        return ArgumentIssue(
            "policy.execution_mode",
            "evaluator-authorized commands use foreground bash");
      }
      return std::nullopt;
    };
    tool.description +=
        " Only an evaluator-authorized exact command is allowed.";
    return false;
  });
}

}  // namespace uagent
