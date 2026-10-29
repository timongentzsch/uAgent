// Copyright 2026 Timon Gentzsch

#include "include/tools/tool.h"

#include <sys/wait.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/core/defaults.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/strings.h"
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

ToolArgumentIssue ArgumentIssue(std::string code, std::string message,
                                std::string field) {
  return {std::move(code), std::move(message), std::move(field)};
}

std::string ArtifactHint(const ToolArtifact& artifact) {
  return kCapturedLogMarker + artifact.path + " (" +
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
  // Every failure the model reads starts with the same marker.
  if (!output.starts_with(kToolErrorPrefix)) {
    output.insert(0, kToolErrorPrefix);
  }
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

ToolResult ExitResult(std::string output, int wait_status) {
  ToolResult result =
      ExitedOk(wait_status)
          ? ToolSuccess(std::move(output))
          : ToolFailure(ToolErrorCode::kProcessFailed, std::move(output));
  result.facts.exit = ExitFacts(wait_status);
  result.hint = ExitHint(wait_status, /*show_ok=*/false);
  return result;
}

std::optional<ResultFacts::Exit> ExitFacts(int wait_status) {
  if (WIFEXITED(wait_status)) {
    return ResultFacts::Exit{.code = WEXITSTATUS(wait_status)};
  }
  if (WIFSIGNALED(wait_status)) {
    return ResultFacts::Exit{.signal = WTERMSIG(wait_status)};
  }
  return std::nullopt;
}

std::string ExitHint(int wait_status, bool show_ok) {
  if (WIFEXITED(wait_status)) {
    return WEXITSTATUS(wait_status) != 0 || show_ok
               ? "[exit code " + std::to_string(WEXITSTATUS(wait_status)) + "]"
               : "";
  }
  if (WIFSIGNALED(wait_status)) {
    return "[killed by signal " + std::to_string(WTERMSIG(wait_status)) + "]";
  }
  return "";
}

ToolResult ChangeResult(std::string output, std::string diff, int64_t added,
                        int64_t removed) {
  ToolResult result = ToolSuccess(std::move(output));
  result.diff = std::move(diff);
  result.body_kind = "diff";
  result.facts.added = added;
  result.facts.removed = removed;
  return result;
}

ToolResult ListResult(std::string output, int64_t items, std::string unit) {
  ToolResult result = ToolSuccess(std::move(output));
  result.facts.items = items;
  result.facts.unit = std::move(unit);
  return result;
}

void AddHint(ToolResult& result, std::string_view line) {
  line.remove_prefix(std::min(line.find_first_not_of('\n'), line.size()));
  if (line.empty()) return;
  if (!result.hint.empty()) result.hint += '\n';
  result.hint += line;
}

std::string ModelHint(const ToolResult& result) {
  std::string hint = result.hint.empty() ? "" : "\n" + result.hint;
  if (result.artifact) hint += ArtifactHint(*result.artifact);
  // A hint alone does not start on an empty line.
  if (result.output.empty() && !hint.empty()) hint.erase(0, 1);
  return hint;
}

std::string ModelText(const ToolResult& result) {
  return result.output + ModelHint(result);
}

const char* ResultState(CompletionStatus status) {
  switch (status) {
    case CompletionStatus::kSuccess:
      return "ok";
    case CompletionStatus::kCancelled:
      return "cancelled";
    case CompletionStatus::kFailed:
    case CompletionStatus::kTimedOut:
      return "failed";
  }
  return "failed";
}

const char* RowState(const ToolResult& result) {
  return result.background ? "running" : ResultState(result.status);
}

std::string ResultWord(const ResultFacts& facts, std::string_view state,
                       double duration_ms) {
  std::string word;
  if (!facts.outcome.empty()) {
    word = facts.outcome;
    std::ranges::replace(word, '_', ' ');
    if (!facts.reason.empty()) word += ": " + facts.reason;
  } else if (facts.exit && facts.exit->signal != 0) {
    word = "killed";
  } else if (facts.exit && facts.exit->code != 0) {
    word = "exit " + std::to_string(facts.exit->code);
  } else if (facts.added.value_or(0) != 0 || facts.removed.value_or(0) != 0) {
    word = "+" + std::to_string(facts.added.value_or(0)) + " −" +
           std::to_string(facts.removed.value_or(0));
  } else if (facts.no_change) {
    word = "no change";
  } else if (facts.items) {
    word = Plural(*facts.items, facts.unit);
  } else if (state == "failed" || state == "cancelled") {
    word = state;
  }
  if (duration_ms < 0) return word;
  std::string took = FmtDuration(duration_ms / 1000);
  return word.empty() ? took : word + " · " + took;
}

json ResultJson(const ResultFacts& facts, std::string_view state,
                double duration_ms) {
  json result = {{"word", ResultWord(facts, state, duration_ms)}};
  if (facts.exit) {
    result["exit"] = facts.exit->signal != 0
                         ? json{{"signal", facts.exit->signal}}
                         : json{{"code", facts.exit->code}};
  }
  if (facts.added) result["added"] = *facts.added;
  if (facts.removed) result["removed"] = *facts.removed;
  if (facts.items) {
    result["items"] = *facts.items;
    result["unit"] = facts.unit;
  }
  if (!facts.outcome.empty()) result["outcome"] = facts.outcome;
  if (!facts.reason.empty()) result["reason"] = facts.reason;
  if (facts.no_change) result["no_change"] = true;
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
  if (tool.name == "memory" || tool.name == "skill") return "memory";
  if (tool.name == "run" || tool.name == "scratch" || tool.name == "activity") {
    return "execute";
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
           category == "system" || tool.name == "activity";
  }
  static const std::unordered_set<std::string> kMinimal = {"read_path", "grep",
                                                           "run", "activity"};
  return category == "system" || kMinimal.contains(tool.name);
}

}  // namespace

bool ToolSelection::Enabled(const Tool& tool) const {
  if (only_ && !only_->contains(tool.name)) return false;
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
    const Tool* tool = FindTool(tools, name);
    if (!tool || (only_ && !only_->contains(name))) {
      error = "unknown or unavailable tool";
      return false;
    }
    auto found = request.find("active");
    if (found == request.end() || !found->is_boolean()) {
      error = "tool active state must be boolean";
      return false;
    }
    const bool active = found->get<bool>();
    if (active == ProfileEnabled(profile_, *tool)) {
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
  only_.reset();
  if (const json* only = JsonArray(value, "only")) {
    only_.emplace();
    for (const json& name : *only) {
      if (name.is_string()) only_->insert(name.get<std::string>());
    }
  }
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
  json result = {{"profile", profile_}, {"overrides", std::move(overrides)}};
  if (only_) {
    std::vector<std::string> names(only_->begin(), only_->end());
    std::ranges::sort(names);
    result["only"] = std::move(names);
  }
  return result;
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
         {"available", !only_ || only_->contains(tool.name)},
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
const json& CommandIntents() {
  static const json kIntents = {"explore", "research", "edit",
                                "verify",  "run",      "setup"};
  return kIntents;
}

namespace {
// A shell line that only looks: every segment of a pipeline or command list
// starts with a read-only program. Anything else, or anything unsure, runs.
bool ReadOnlyCommand(const std::string& command) {
  // Programs with no mode that changes anything (find and fd can -delete or
  // -exec, git branch can -D, so they run).
  static const std::set<std::string, std::less<>> kLooks = {
      "cat", "cd", "du",   "echo", "file", "grep", "head", "ls",
      "pwd", "rg", "stat", "tail", "tree", "wc",   "which"};
  static const std::set<std::string, std::less<>> kGitLooks = {
      "blame", "diff", "grep", "log", "ls-files", "show", "status"};
  if (command.find_first_of("<>`$") != std::string::npos) return false;
  bool any = false;
  size_t start = 0;
  while (start <= command.size()) {
    size_t end = command.find_first_of("|;&\n", start);
    if (end == std::string::npos) end = command.size();
    std::istringstream words(command.substr(start, end - start));
    std::string program, sub;
    words >> program >> sub;
    if (!program.empty()) {
      if (program == "git" ? !kGitLooks.contains(sub)
                           : !kLooks.contains(program)) {
        return false;
      }
      any = true;
    }
    start = command.find_first_not_of("|;&\n", end);
    if (start == std::string::npos) break;
  }
  return any;
}
}  // namespace

std::string ToolActivityCategory(const Tool& tool, const json& args) {
  if (tool.declared_intent) {
    std::string intent = JsonValue(args, "intent", "");
    for (const json& known : CommandIntents()) {
      if (known == intent) return intent;
    }
    return ReadOnlyCommand(JsonValue(args, "command", "")) ? "explore" : "run";
  }
  if (!tool.intent.empty()) return tool.intent;
  if (tool.capabilities & Capability(ToolCapability::kDelegate)) {
    return "delegate";
  }
  if (tool.capabilities & Capability(ToolCapability::kExecute)) return "run";
  return ToolMutates(tool, args) ? "edit" : "explore";
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

std::string ToolDescription(const Tool& tool) {
  std::string s = tool.description;
  const ActionRequires& table = tool.action_requires;
  for (size_t row = 0; row < table.actions.size(); ++row) {
    const auto& [action, fields] = table.actions[row];
    s += row == 0 ? " Required per " + table.field + ": " : "; ";
    s += action + ":";
    for (size_t index = 0; index < fields.size(); ++index) {
      s += (index == 0 ? " " : ", ") + fields[index];
    }
    if (row + 1 == table.actions.size()) s += ".";
  }
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

std::string DefaultTarget(const json& args) {
  constexpr size_t kTargetChars = 160;
  if (!args.is_object()) return {};
  for (const auto& [key, value] : args.items()) {
    if (key == "intent" || !value.is_string()) continue;
    const std::string& text = value.get_ref<const std::string&>();
    if (!text.empty() && text.size() <= kTargetChars &&
        text.find('\n') == std::string::npos) {
      return text;
    }
  }
  return {};
}

Headline ToolHeadline(const Tool* tool, const json& args) {
  if (tool && tool->headline) return tool->headline(args);
  const std::string title = tool ? ToolTitle(*tool) : "tool";
  return {"Calling " + title, "Called " + title, DefaultTarget(args)};
}

std::string ToolSummary(const Tool& t, const json& args) {
  Headline headline = ToolHeadline(&t, args);
  return headline.target.empty() ? headline.past
                                 : headline.past + " " + headline.target;
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
    if (key == "intent" || value.is_null() ||
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

json LinkPart(std::string to, json id, std::string label) {
  return {{"kind", "link"},
          {"to", std::move(to)},
          {"id", std::move(id)},
          {"label", std::move(label)}};
}

Tool::Headlines Verbs(std::string present, std::string past) {
  return
      [present = std::move(present), past = std::move(past)](const json& args) {
        return Headline{present, past, DefaultTarget(args)};
      };
}

json ToolView(const Tool* tool, const json& args) {
  Headline headline = ToolHeadline(tool, args);
  json view = {
      {"verb", {std::move(headline.present), std::move(headline.past)}},
      {"input",
       tool && tool->present ? tool->present(args) : GenericInputParts(args)}};
  if (!headline.target.empty()) view["target"] = std::move(headline.target);
  return view;
}

const Tool* FindTool(const std::vector<Tool>& tools, const std::string& name) {
  for (auto& t : tools) {
    if (t.name == name) return &t;
  }
  return nullptr;
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
