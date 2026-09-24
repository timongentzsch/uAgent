// Copyright 2026 Timon Gentzsch

#include "include/tools/tool.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/time.h"

namespace uagent {

const char* ToolErrorCodeName(ToolErrorCode code) {
  switch (code) {
    case ToolErrorCode::kNone:
      return "none";
    case ToolErrorCode::kInvalidArguments:
      return "invalid_arguments";
    case ToolErrorCode::kPermissionDenied:
      return "permission_denied";
    case ToolErrorCode::kNotFound:
      return "not_found";
    case ToolErrorCode::kLimitExceeded:
      return "limit_exceeded";
    case ToolErrorCode::kUnavailable:
      return "unavailable";
    case ToolErrorCode::kProcessFailed:
      return "process_failed";
    case ToolErrorCode::kRemoteError:
      return "remote_error";
    case ToolErrorCode::kInternal:
      return "internal";
  }
  return "internal";
}

std::string ToolErrorText(std::string_view message) {
  std::string out(kToolErrorPrefix);
  out += message;
  return out;
}

ToolArgumentIssue ArgumentIssue(std::string code, std::string message,
                                std::string field) {
  return {std::move(code), std::move(message), std::move(field)};
}

std::string ArtifactHint(const ToolArtifact& artifact) {
  return "\n[captured log: " + artifact.path + " (" +
         std::to_string(artifact.bytes) +
         " bytes); query with jq/python via run or read selected ranges; do "
         "not read it whole]";
}

ToolResult ToolSuccess(std::string output, int64_t result_chars) {
  ToolResult result;
  result.output = std::move(output);
  result.result_chars = result_chars;
  return result;
}

ToolResult ToolFailure(ToolErrorCode error, std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kFailed;
  result.output = std::move(output);
  result.error = error;
  return result;
}

ToolResult ToolCancelled(std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kCancelled;
  result.output = std::move(output);
  return result;
}

ToolResult ToolTimedOut(std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kTimedOut;
  result.output = std::move(output);
  return result;
}

bool ToolContext::Expired() const {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

ToolContext ToolContext::WithTimeout(int64_t seconds) const {
  ToolContext out = *this;
  out.timeout_s = std::max(int64_t{0}, seconds);
  if (seconds > 0) {
    out.deadline = std::min(out.deadline, DeadlineAfter(seconds));
  }
  return out;
}

int64_t ToolContext::RemainingSeconds(int64_t configured) const {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return configured;
  }
  int64_t remaining =
      static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count());
  remaining = std::max(int64_t{1}, remaining);
  return configured > 0 ? std::min(configured, remaining) : remaining;
}

Tool MakeTool(std::string name, std::string description, json parameters,
              Tool::Run run) {
  Tool tool;
  tool.name = std::move(name);
  tool.description = std::move(description);
  tool.parameters = std::move(parameters);
  tool.run = std::move(run);
  return tool;
}

std::string ToolTitle(const Tool& tool) {
  if (!tool.title.empty()) return tool.title;
  std::string title = tool.name;
  bool capitalize = true;
  for (char& ch : title) {
    if (ch == '_' || ch == '-') {
      ch = ' ';
      capitalize = true;
    } else if (capitalize) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      capitalize = false;
    }
  }
  return title;
}

std::string ToolCategory(const Tool& tool) {
  if (!tool.category.empty()) return tool.category;
  if (tool.provider.starts_with("mcp:")) return "mcp";
  if (tool.name == "web_search" || tool.name == "web_fetch") return "web";
  if (tool.name == "subagent") return "collaborate";
  if (tool.name == "memory" || tool.name == "skill") return "memory";
  if (tool.name == "run" || tool.name == "scratch" || tool.name == "activity") {
    return "execute";
  }
  if (tool.name == "uagent" || tool.name == "session" ||
      tool.name == "adapt_system") {
    return "system";
  }
  return "workspace";
}

namespace {

bool ValidProfile(std::string_view profile) {
  return profile == "default" || profile == "coding" || profile == "research" ||
         profile == "minimal";
}

bool ProfileEnabled(std::string_view profile, const Tool& tool) {
  if (profile == "default") return true;
  const std::string category = ToolCategory(tool);
  if (profile == "coding") {
    return category != "web" && category != "collaborate";
  }
  if (profile == "research") {
    return category == "workspace" || category == "web" ||
           category == "collaborate" || category == "memory" ||
           tool.name == "activity" || tool.name == "uagent" ||
           tool.name == "session";
  }
  static const std::unordered_set<std::string> kMinimal = {
      "read_path", "grep", "run", "activity", "uagent", "session"};
  return kMinimal.contains(tool.name);
}

bool KnownTool(const std::vector<Tool>& tools, std::string_view name) {
  return std::any_of(tools.begin(), tools.end(),
                     [&](const Tool& tool) { return tool.name == name; });
}

}  // namespace

bool ToolSelection::Enabled(const Tool& tool) const {
  auto override = overrides_.find(tool.name);
  return override == overrides_.end() ? ProfileEnabled(profile_, tool)
                                      : override->second;
}

bool ToolSelection::Configure(const json& request,
                              const std::vector<Tool>& tools,
                              std::string& error) {
  const std::string operation = JsonValue(request, "operation", "catalog");
  if (operation == "catalog") return true;
  if (operation == "reset") {
    profile_ = "default";
    overrides_.clear();
    return true;
  }
  if (operation == "profile") {
    const std::string profile = JsonValue(request, "profile", "");
    if (!ValidProfile(profile)) {
      error = "unknown tool profile";
      return false;
    }
    profile_ = profile;
    overrides_.clear();
    return true;
  }
  if (operation == "set") {
    const std::string name = JsonValue(request, "name", "");
    if (!KnownTool(tools, name)) {
      error = "unknown or unavailable tool";
      return false;
    }
    auto found = request.find("active");
    if (found == request.end() || !found->is_boolean()) {
      error = "tool active state must be boolean";
      return false;
    }
    const Tool* tool = FindTool(tools, name);
    const bool active = found->get<bool>();
    if (tool && active == ProfileEnabled(profile_, *tool)) {
      overrides_.erase(name);
    } else {
      overrides_[name] = active;
    }
    return true;
  }
  error = "unknown tool operation";
  return false;
}

void ToolSelection::Restore(const json& value) {
  profile_ = JsonValue(value, "profile", "default");
  if (!ValidProfile(profile_)) profile_ = "default";
  overrides_.clear();
  const json overrides = JsonValue(value, "overrides", json::object());
  if (!overrides.is_object()) return;
  for (const auto& [name, active] : overrides.items()) {
    if (overrides_.size() >= kMaxToolSelectionOverrides) break;
    if (!name.empty() && name.size() <= kToolNameChars && active.is_boolean()) {
      overrides_[name] = active.get<bool>();
    }
  }
}

json ToolSelection::Save() const {
  json overrides = json::object();
  for (const auto& [name, active] : overrides_) overrides[name] = active;
  return {{"profile", profile_}, {"overrides", std::move(overrides)}};
}

json ToolSelection::Catalogue(const std::vector<Tool>& tools) const {
  json rows = json::array();
  json active_schemas = json::array();
  json full_schemas = json::array();
  int64_t active_count = 0;
  for (const Tool& tool : tools) {
    json schema = ToolSchema(tool);
    const size_t bytes = JsonDump(schema).size();
    const bool active = Enabled(tool);
    full_schemas.push_back(schema);
    if (active) {
      ++active_count;
      active_schemas.push_back(std::move(schema));
    }
    rows.push_back(
        {{"name", tool.name},
         {"title", ToolTitle(tool)},
         {"description", tool.description},
         {"category", ToolCategory(tool)},
         {"provider", tool.provider.empty() ? "builtin" : tool.provider},
         {"active", active},
         {"available", true},
         {"schema_bytes", bytes}});
  }
  return {
      {"profile", overrides_.empty() ? profile_ : "custom"},
      {"base_profile", profile_},
      {"profiles", json::array({"default", "coding", "research", "minimal"})},
      {"active", active_count},
      {"available", static_cast<int64_t>(tools.size())},
      {"schema_bytes", static_cast<int64_t>(JsonDump(active_schemas).size())},
      {"full_schema_bytes",
       static_cast<int64_t>(JsonDump(full_schemas).size())},
      {"tools", std::move(rows)}};
}

Tool& AddTool(std::vector<Tool>& tools, Tool tool) {
  tools.push_back(std::move(tool));
  return tools.back();
}

bool ToolMutates(const Tool& tool, const json& arguments) {
  return tool.mutating || (tool.mutates && tool.mutates(arguments));
}

// Repeat-guard exemption for Agent::ToolCallsWithinLimits: a call that
// deliberately blocks — activity wait_ms or run yield_ms — is waiting for
// something to finish, not stuck in a tight identical-call loop, so it
// resets the counter instead of tripping it.
bool ToolCallBlocks(const Tool& tool, const json& arguments) {
  if (tool.blocking_wait_default_ms >= 0 &&
      JsonValue(arguments, "wait_ms", tool.blocking_wait_default_ms) > 0) {
    return true;
  }
  return JsonValue(arguments, "yield_ms", int64_t{0}) > 0;
}

// Contract-defined for native operations; arbitrary execution may declare its
// purpose. Neither this label nor a successful exit proves absence of effects.
std::string ToolActivityCategory(const Tool& tool, const json& args) {
  if (tool.declared_intent) {
    std::string intent = JsonValue(args, "intent", "execute");
    return intent == "explore" || intent == "change" ? intent : "execute";
  }
  if (tool.capabilities & (Capability(ToolCapability::kExecute) |
                           Capability(ToolCapability::kDelegate))) {
    return "execute";
  }
  return ToolMutates(tool, args) ? "change" : "explore";
}

// The authority a call needs. A tool may escalate specific arguments; nothing
// can de-escalate below what the tool itself declares.
ApprovalClass RequiredApproval(const Tool& tool, const json& arguments) {
  if (tool.approval_class) {
    ApprovalClass escalated = tool.approval_class(arguments);
    if (escalated == ApprovalClass::kMandatoryHuman) return escalated;
  }
  bool required = ToolMutates(tool, arguments) ||
                  (tool.needs_approval && tool.needs_approval(arguments));
  return required ? ApprovalClass::kYoloEligibleMutation : ApprovalClass::kNone;
}

void KeepLeanTools(std::vector<Tool>& tools) {
  std::erase_if(tools,
                [](const Tool& tool) { return !tool.available_in_lean; });
}

std::string ToolDescription(const Tool& tool) {
  std::string s = tool.description;
  // Mark tools that actually overlap, so the base prompt's batching rule is
  // actionable.
  if (tool.parallel_safe) s += " Batchable with independent calls.";
  if (tool.max_calls_per_turn >= 0) {
    s += " Limit: " + std::to_string(tool.max_calls_per_turn) + "/turn.";
  }
  return s;
}

json ToolParameters(const Tool& tool) {
  if (tool.provider.starts_with("mcp:")) return tool.parameters;
  json parameters = tool.parameters;
  if (!parameters.is_object()) parameters = json::object();
  if (!parameters.contains("type")) parameters["type"] = "object";
  if (!parameters.contains("properties") ||
      !parameters["properties"].is_object()) {
    parameters["properties"] = json::object();
  }
  if (!parameters.contains("additionalProperties")) {
    parameters["additionalProperties"] = false;
  }
  return parameters;
}

// One-line display for a call: the tool's own formatter, else `path` (the
// common case), else the raw args. Shared by the approval prompt and the
// call trace so both name the same action the same way.
std::string ToolSummary(const Tool& t, const json& args) {
  if (t.summary) return t.summary(args);
  if (args.contains("path") && args["path"].is_string()) {
    return args["path"].get<std::string>();
  }
  return JsonDump(args);
}

json CommandPart(std::string text) {
  return {{"kind", "command"},
          {"text", Utf8Trunc(std::move(text), kPreviewChars)}};
}

json CodePart(std::string text, std::string language, std::string label) {
  json part = {{"kind", "code"},
               {"text", Utf8Trunc(std::move(text), kPreviewChars)},
               {"language", std::move(language)}};
  if (!label.empty()) part["label"] = std::move(label);
  return part;
}

json GenericInputParts(const json& args,
                       std::initializer_list<std::string_view> skip) {
  json parts = json::array();
  if (!args.is_object()) {
    if (!args.is_null()) {
      parts.push_back(
          CodePart(args.is_string() ? args.get<std::string>() : args.dump(2),
                   args.is_string() ? "" : "json"));
    }
    return parts;
  }
  constexpr size_t kFieldChars = 160;
  json rows = json::array();
  for (const auto& [key, value] : args.items()) {
    if (key == "intent" || key == "description" || value.is_null() ||
        std::find(skip.begin(), skip.end(), key) != skip.end()) {
      continue;
    }
    if (value.is_string()) {
      const std::string& text = value.get_ref<const std::string&>();
      if (text.size() <= kFieldChars && text.find('\n') == std::string::npos) {
        rows.push_back(json::array({key, text}));
      } else {
        parts.push_back(CodePart(text, "", key));
      }
    } else if (value.is_primitive()) {
      rows.push_back(json::array({key, value.dump()}));
    } else {
      std::string text = value.dump(2);
      if (text.size() <= kFieldChars && text.find('\n') == std::string::npos) {
        rows.push_back(json::array({key, std::move(text)}));
      } else {
        parts.push_back(CodePart(std::move(text), "json", key));
      }
    }
  }
  if (!rows.empty()) {
    parts.insert(parts.begin(),
                 json{{"kind", "fields"}, {"rows", std::move(rows)}});
  }
  return parts;
}

json ToolView(const Tool* tool, const json& args) {
  return {{"input", tool && tool->present ? tool->present(args)
                                          : GenericInputParts(args)},
          {"output", tool && tool->markdown_output ? "markdown" : "text"}};
}

const Tool* FindTool(const std::vector<Tool>& tools, const std::string& name) {
  for (auto& t : tools) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

std::string InvalidToolArgument(const Tool& tool, const json& args) {
  auto issue = FindToolArgumentIssue(tool, args);
  return issue ? issue->message : std::string();
}

json ToolSchema(const Tool& tool) {
  return {{"type", "function"},
          {"function",
           {{"name", tool.name},
            {"description", ToolDescription(tool)},
            {"parameters", ToolParameters(tool)}}}};
}

// registry -> the `tools` array for a chat request
json ToolSchemas(const std::vector<Tool>& tools) {
  json out = json::array();
  for (const Tool& tool : tools) {
    out.push_back(ToolSchema(tool));
  }
  return out;
}

}  // namespace uagent
