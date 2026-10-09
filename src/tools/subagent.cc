// Copyright 2026 Timon Gentzsch

#include "include/tools/subagent.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/mailbox.h"
#include "include/core/strings.h"
#include "include/tools/shell.h"

namespace uagent {
namespace {

// Enough to show the shape of a configured roster without charging the whole
// list to every request; uagent action=inspect topic=routes reports all of
// them.
constexpr size_t kAdvertisedRoutes = 4;
constexpr size_t kAgentNameMax = 32;
constexpr size_t kAgentDescriptionMax = 280;
// The role rides in the session header, which is read through a bounded
// prefix; the brief itself lives in the conversation.
constexpr size_t kAgentDirectiveMax = 4096;

// A child is a session file in this workspace's history, named by its id.
std::string AgentPath(const std::string& id) {
  return UagentDir(kHistoryDir) + "/" + WorkspaceId(CanonicalCwd()) + "/" + id +
         ".json";
}

// Short because the id is quoted back in every spawn, followup, message and
// list result -- a recurring token cost for something no human types. The
// digest is the same FNV-1a construction session ids use, truncated to eight
// hex digits and retried against the file it would name. Retrying is not
// exclusive creation: two processes can still agree on a free id at the same
// instant. That residual is accepted -- the inputs already include the pid.
std::string NewAgentId() {
  const std::string seed = UniqueSeed();
  std::error_code code;
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::string id =
        "agent-" +
        TruncatedHash(seed + ":" + std::to_string(attempt), kAgentNameChars);
    if (!std::filesystem::exists(AgentPath(id), code)) return id;
  }
  // Eight collisions in a row is a broken digest, not bad luck. Fall back to
  // the full width rather than handing back an id that is known to be taken.
  return "agent-" + HashHex(seed);
}

bool ValidAgentName(std::string_view name) {
  if (name.empty() || name.size() > kAgentNameMax) return false;
  if (name.front() == '-' || name.back() == '-') return false;
  for (char c : name) {
    const bool lower = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    if (!lower && !digit && c != '-') return false;
  }
  return true;
}

std::optional<int64_t> RunningAgent(const ProcessSupervisor& processes,
                                    const std::string& id) {
  for (const BgJob& job : processes.Snapshot()) {
    if (job.source_id == id) return ActivityId(job);
  }
  return std::nullopt;
}

// The child's role from its session header, when this session is its parent.
// A child still running for this session is ours before its first save.
bool LoadRole(const ProcessSupervisor& processes, const std::string& id,
              json& role, std::string& error) {
  if (id.empty() || SafeFileComponent(id) != id) {
    error = "invalid agent id";
    return false;
  }
  role = JsonValue(SessionHeader(AgentPath(id)), kSessionHeaderDelegation,
                   json::object());
  if (role.empty()) {
    if (RunningAgent(processes, id)) return true;
    error = "agent not found";
    return false;
  }
  if (JsonValue(role, "parent", "") != processes.Owner()) {
    error = "agent belongs to another conversation";
    return false;
  }
  return true;
}

json AgentRow(const std::string& id, const json& role) {
  return {{"id", id},
          {"name", JsonValue(role, "name", "")},
          {"description", JsonValue(role, "description", "")},
          {"model", JsonValue(role, "route", JsonValue(role, "model", ""))},
          {"label", JsonValue(role, "label", "Subagent")},
          {"mode", JsonValue(role, "mode", "lean")},
          {"status", "idle"}};
}

}  // namespace

std::vector<json> AgentSummaries(const ProcessSupervisor& processes) {
  std::vector<json> rows;
  for (const SessionInfo& info : ListSessions(SessionScope::kChildren)) {
    if (rows.size() >= kMaxAgentRecords) break;
    if (JsonValue(info.delegation, "parent", "") != processes.Owner()) continue;
    rows.push_back(AgentRow(std::filesystem::path(info.path).stem().string(),
                            info.delegation));
  }
  // The supervisor knows what runs; a child that has not saved yet is listed
  // from what its spawn stamped on the job.
  for (const BgJob& job : processes.Snapshot()) {
    if (job.kind != ActivityKind::kSubagent || job.source_id.empty()) continue;
    auto row = std::find_if(rows.begin(), rows.end(), [&](const json& item) {
      return JsonValue(item, "id", "") == job.source_id;
    });
    if (row == rows.end()) {
      row = rows.insert(rows.end(), AgentRow(job.source_id, job.metadata));
    }
    (*row)["status"] = "running";
    // The handle the activity tool wants, so a caller that sees "running"
    // does not have to guess at one to wait on or stop it.
    (*row)["activity"] = ActivityId(job);
  }
  return rows;
}

ToolResult MessageAgent(const ProcessSupervisor& processes,
                        const std::string& id, const std::string& text) {
  json role;
  std::string error;
  if (!LoadRole(processes, id, role, error)) {
    return ToolFailure(ToolErrorCode::kNotFound, error);
  }
  if (text.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "message requires text");
  }
  // A parent without a session file is known to its child by its owner id.
  const std::string own = OwnSessionFile();
  Mail mail;
  mail.from = own.empty() ? processes.Owner() : MailboxIdFor(own);
  mail.sender_path = own;
  mail.to = MailboxIdFor(AgentPath(id));
  mail.type = kMailSteer;
  mail.body = {{"text", "[parent guidance]\n" + text}};
  error = SendMail(std::move(mail));
  return error.empty() ? ToolSuccess("sent to agent " + id)
                       : ToolFailure(ToolErrorCode::kUnavailable, error);
}

json InspectAgent(const ProcessSupervisor& processes, const std::string& id,
                  const json& request) {
  json role;
  std::string error;
  if (!LoadRole(processes, id, role, error)) return {{"error", error}};
  json detail = {{"agent_id", id},
                 {"name", JsonValue(role, "name", "")},
                 {"description", JsonValue(role, "description", "")},
                 {"label", JsonValue(role, "label", "Subagent")},
                 {"directive", JsonValue(role, "directive", "")},
                 {"route", JsonValue(role, "route", "")}};
  auto loaded = SessionStore::Inspect(AgentPath(id));
  if (!loaded.record) return detail;
  auto& record = *loaded.record;
  Conversation conversation;
  if (!std::move(record.state).RestoreConversation(conversation)) {
    return {{"error", "invalid child conversation"}};
  }
  const std::string message = JsonValue(request, "detail", "");
  if (!message.empty()) {
    const size_t offset = JsonValue(request, "offset", size_t{0});
    json body = JsonValue(request, "raw", false)
                    ? ConversationExchange(conversation, message, offset)
                    : ConversationDetail(conversation, message, offset);
    return body.contains("error") ? body : json{{"body", std::move(body)}};
  }
  detail.update({{"conversation",
                  ConversationView(conversation,
                                   JsonValue(request, "before", uint64_t{0}))},
                 {"turns", record.metadata.turns},
                 {"model", record.metadata.model},
                 {"context_tokens", record.state.context_tokens},
                 {"context_window", record.state.context_window},
                 {"statistics", conversation.Statistics()},
                 {"usage", UsageJson(record.state.usage)}});
  if (!record.state.last_sent_prompt.empty()) {
    detail["system_prompt"] = record.state.last_sent_prompt;
  }
  return detail;
}

namespace {

std::string JoinSelections(std::vector<std::string> selections) {
  std::sort(selections.begin(), selections.end());
  selections.erase(std::unique(selections.begin(), selections.end()),
                   selections.end());
  size_t total = selections.size();
  if (selections.size() > kAdvertisedRoutes) {
    selections.resize(kAdvertisedRoutes);
  }
  std::string result;
  for (const std::string& selection : selections) {
    if (!result.empty()) result += ", ";
    result += selection;
  }
  if (total > selections.size()) {
    result += ", +" + std::to_string(total - selections.size()) + " more";
  }
  return result;
}

// The per-child ceilings live under one `limits` object. An absent object and
// an absent field mean the same thing -- inherit the configured default -- so
// every reader goes through here rather than testing for the object first.
const json& ChildLimits(const json& arguments) {
  static const json kNone = json::object();
  const json* limits = JsonObject(arguments, "limits");
  return limits != nullptr ? *limits : kNone;
}

std::string ModelPropertyDescription(
    const std::vector<ModelRoute>& routes,
    const std::vector<NamedProvider>& providers) {
  std::vector<std::string> aliases;
  aliases.reserve(routes.size());
  for (const ModelRoute& route : routes) aliases.push_back(route.name);

  // Naming an alias here is an override, not the default. Spelling that out
  // matters: a child sent to an unreachable alias fails outright rather than
  // falling back, so the default must read as the safe choice.
  std::string description = "child route; omit to run it on your own";
  std::string configured = JoinSelections(std::move(aliases));
  if (!configured.empty()) {
    description += " Overrides: " + configured + ".";
  }
  // The grammar, not the enumeration. Naming every provider here charged a
  // list to every request; withholding the grammar entirely is what produced
  // guessed selections. The shape stays; uagent inspection reports the roster.
  if (!providers.empty()) {
    description +=
        " Or <provider>/MODEL for a configured provider; uagent "
        "action=inspect topic=routes lists them.";
  }
  return description;
}

std::string SubagentDiagnosticRoute(
    const SideRoute& route, const std::vector<NamedProvider>& providers) {
  std::string selected = route.selection;
  std::string resolved = RouteSelection(route, providers);
  std::string label = selected.empty() ? resolved : selected;
  if (!resolved.empty() && resolved != label) label += " -> " + resolved;
  std::string host = UrlHost(route.base_url);
  if (!host.empty()) label += " @ " + host;
  return label;
}

ToolResult RunSubagent(const Api& api, ProcessSupervisor& processes,
                       const std::vector<ModelRoute>& routes,
                       const std::vector<NamedProvider>& providers, bool debug,
                       const json& arguments, const ToolContext& context) {
  std::string operation = JsonValue(arguments, "operation", "spawn");
  std::string id = JsonValue(arguments, "agent_id", "");
  if (operation == "list") {
    std::vector<json> agents = AgentSummaries(processes);
    return ToolSuccess(agents.empty() ? "no agents" : JsonDump(agents, 2));
  }
  if (operation == "message") {
    if (!JsonValue(arguments, "directive", "").empty()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "message cannot change directive; use "
                         "followup");
    }
    // A running child reads it at its next step; a finished one
    // runs again on it.
    if (RunningAgent(processes, id)) {
      return MessageAgent(processes, id, JsonValue(arguments, "prompt", ""));
    }
    operation = "followup";
  }
  if (operation != "spawn" && operation != "followup") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "operation must be spawn, followup, "
                       "message, or list");
  }
  // The child's role, written into its session header by the child.
  json role = json::object();
  if (operation == "spawn") {
    if (!id.empty()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "agent_id is assigned by spawn");
    }
    id = NewAgentId();
  } else {
    std::string error;
    if (!LoadRole(processes, id, role, error)) {
      return ToolFailure(ToolErrorCode::kNotFound, error);
    }
    if (std::optional<int64_t> active = RunningAgent(processes, id)) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "agent " + id + " is already running as activity " +
                             std::to_string(*active));
    }
  }
  // An empty value is one that was not given: models that fill in every
  // optional field would otherwise wipe what an earlier call had set.
  for (const char* field : {"name", "description", "directive"}) {
    std::string given = JsonValue(arguments, field, "");
    if (!given.empty()) role[field] = std::move(given);
  }
  const std::string name = JsonValue(role, "name", "");
  if (!name.empty() && !ValidAgentName(name)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "name must match [a-z0-9-]{1,32}, no "
                       "leading/trailing '-'");
  }
  role["description"] =
      Utf8Trunc(JsonValue(role, "description", ""), kAgentDescriptionMax);
  if (JsonValue(role, "directive", "").size() > kAgentDirectiveMax) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "directive is limited to " +
                           std::to_string(kAgentDirectiveMax) + " bytes");
  }

  std::string prompt = JsonValue(arguments, "prompt", "");
  if (prompt.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "spawn or followup requires prompt");
  }
  const std::string directive = JsonValue(role, "directive", "");
  if (!directive.empty()) {
    prompt = "[collaborator directive]\n" + directive + "\n\n" + prompt;
  }
  const std::string mode =
      JsonValue(arguments, "mode", JsonValue(role, "mode", "lean"));
  if (mode != "lean" && mode != "full") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "mode must be lean or full");
  }
  const std::string requested = NormalizeModelId(
      JsonValue(arguments, "model", JsonValue(role, "model", "")));
  // No route named: the parent's own, whatever it is now.
  SideRoute route = ResolveSideRoute(api, routes, providers, requested);
  const std::string route_label = SubagentDiagnosticRoute(route, providers);
  if (route.unresolved && route.selection.find('/') != std::string::npos &&
      !CanUseRawModel(api, route.selection)) {
    return ToolFailure(
        ToolErrorCode::kInvalidArguments,
        ChildAgentFailureReport(
            route_label, ChildAgentFailureStage::kRouteResolution,
            "unknown model route: " + TerminalSafe(route.selection)));
  }
  double remaining_budget = 0;
  int64_t remaining_token_budget = 0;
  if (std::optional<ToolResult> blocked = ChildAgentBudgetBlock(
          api, processes, remaining_budget, remaining_token_budget)) {
    return *blocked;
  }
  const std::string child_model = route.model;
  EnvironmentOverrides environment = ChildAgentEnvironment(std::move(route));
  // A caller that knows the shape of the subtask may raise or lower the
  // ceiling for that one child; the schema bounds it, and the session
  // budgets still apply underneath.
  const json& limits = ChildLimits(arguments);
  const int64_t steps = JsonValue(limits, "steps", SubagentMaxSteps());
  const int64_t tool_calls =
      JsonValue(limits, "tool_calls", SubagentMaxToolCalls());
  const bool background = JsonValue(arguments, "background", false);
  // A caller may deny memory but not grant it: the session decides what
  // this process may read, and a child cannot widen that.
  const bool child_memory =
      api.config.memory_enabled &&
      JsonValue(limits, "memory", JsonValue(role, "memory", true));
  role.update(
      {{"parent", processes.Owner()},
       {"parent_session", OwnSessionFile()},
       {"mode", mode},
       {"model", requested},
       {"route", route_label},
       {"label", Utf8Trunc(FirstLine(JsonValue(arguments, "prompt", "")), 160)},
       {"memory", child_memory}});
  environment.insert(
      environment.end(),
      {{"UAGENT_MAX_STEPS", std::to_string(steps)},
       {"UAGENT_MAX_TOOL_CALLS", std::to_string(tool_calls)},
       {"UAGENT_INTERNAL_TOOLSET", mode},
       {"UAGENT_INTERNAL_PARENT_TURN", std::to_string(context.turn_id)},
       {"UAGENT_MEMORY", child_memory ? "1" : "0"},
       {"UAGENT_INTERNAL_SESSION_FILE", AgentPath(id)},
       {"UAGENT_INTERNAL_DELEGATION", JsonDump(role)}});
  // A tool this session has switched off is not handed to its child either:
  // the child's toolset is cut down to what the session itself may call.
  if (context.enabled_tools.empty()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "every tool is switched off in this conversation, so a "
                       "child would have none");
  }
  environment.emplace_back("UAGENT_INTERNAL_TOOL_ALLOWLIST",
                           JsonDump(json(context.enabled_tools)));
  // Only a background child is polled while it runs. A foreground child
  // is read once, where progress lines would only pad the answer the
  // parent quotes.
  if (background) {
    environment.emplace_back("UAGENT_INTERNAL_HEADLESS_PROGRESS", "1");
  }
  // Tightening is the caller's to do; loosening is not. A requested
  // budget above what the session has left is clamped, and the clamp is
  // reported rather than applied behind the caller's back.
  std::vector<std::string> clamped;
  double child_budget = JsonValue(limits, "cost", 0.0);
  if (api.config.session_budget > 0) {
    if (child_budget <= 0 || child_budget > remaining_budget) {
      if (child_budget > remaining_budget) {
        clamped.push_back("limits.cost to " + FmtCost(remaining_budget) +
                          ", the session's remainder");
      }
      child_budget = remaining_budget;
    }
  }
  if (child_budget > 0) {
    environment.emplace_back("UAGENT_SESSION_BUDGET",
                             std::to_string(child_budget));
  }
  if (api.config.session_token_budget > 0) {
    environment.emplace_back("UAGENT_SESSION_TOKEN_BUDGET",
                             std::to_string(remaining_token_budget));
  }
  // The child keeps its own time, like its steps and tool calls: it is the
  // one that can stop at the limit and still answer, in the background too.
  int64_t max_seconds = JsonValue(limits, "seconds", int64_t{0});
  int64_t ceiling = SubagentTimeoutSeconds();
  if (ceiling > 0 && (max_seconds <= 0 || max_seconds > ceiling)) {
    if (max_seconds > ceiling) {
      clamped.push_back("limits.seconds to " + std::to_string(ceiling) +
                        ", this build's ceiling");
    }
    max_seconds = ceiling;
  }
  if (max_seconds > 0) {
    environment.emplace_back("UAGENT_MAX_TURN_SECONDS",
                             std::to_string(max_seconds));
  }
  ShellCommandResult child = RunShellCommand(
      processes, context,
      {.command = "uagent subagent",
       .argv = ChildAgentCommand(debug, prompt, child_model),
       .background = background,
       .immediate = background,
       // Runs uagent itself, which writes ~/.uagent state a confined child
       // could not. Its own commands run under this session's sandbox.
       .sandbox = false,
       .activity_kind = ActivityKind::kSubagent,
       .activity_label = route_label,
       .source_id = id,
       .completion_notes = clamped,
       .activity_metadata = {{"label", JsonValue(role, "label", "")},
                             {"name", name},
                             {"mode", mode},
                             {"model", route_label}},
       .environment = std::move(environment),
       .environment_policy = ChildEnvironmentPolicy::kAgent});
  ToolResult result = std::move(child.result);
  if (child.wait_status && result.artifact) {
    // The child ran long enough for its log to outgrow the cap, so the
    // text here may begin inside the record it ends with.
    result.output = ChildAgentRecoverEnvelope(std::move(result.output),
                                              result.artifact->path);
  }
  if (result.Ok()) {
    // A launch receipt is process-supervisor output, not a malformed
    // child answer. Only a process that actually completed can have a
    // headless envelope to unwrap; retained completion notes travel with
    // a background job and are added again to its final result.
    result.output =
        child.wait_status
            ? ChildAgentAnswer(std::move(result.output), clamped)
            : std::move(result.output) + ChildAgentConstraintNotes(clamped);
  }
  if (!result.Ok() && result.status != CompletionStatus::kCancelled) {
    ChildAgentFailureStage stage = child.wait_status
                                       ? ChildAgentFailureStage::kExecution
                                       : ChildAgentFailureStage::kSpawn;
    result.output = ChildAgentFailureReport(route_label, stage, result.output) +
                    ChildAgentConstraintNotes(clamped);
  }
  if (child.launched) {
    result.output += ChildAgentResumeNote(id);
    result.parts = json::array({LinkPart("agent", id, "Open agent")});
  }
  return result;
}

}  // namespace

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug) {
  json properties = {
      {"operation",
       {{"type", "string"},
        {"enum", json::array({"spawn", "followup", "message", "list"})},
        {"description",
         "spawn by default; followup, message or list children"}}},
      {"agent_id",
       {{"type", "string"},
        {"description", "child id for followup or message"}}},
      {"name",
       {{"type", "string"},
        {"maxLength", 32},
        {"description", "reusable role, e.g. api-reviewer"}}},
      {"description",
       {{"type", "string"},
        {"maxLength", 280},
        {"description",
         "spawn/followup: 1-2 sentences on expertise and when to reuse"}}},
      {"prompt",
       {{"type", "string"},
        {"description",
         "standalone brief for spawn; next message for a followup or queued "
         "message"}}},
      {"directive",
       {{"type", "string"},
        {"maxLength", kAgentDirectiveMax},
        {"description",
         "persistent coordinator guidance prepended to followups; a new "
         "one replaces it"}}},
      {"background",
       {{"type", "boolean"},
        {"description",
         "default false: wait and get the child's answer directly; true "
         "when several start in one batch or other work is waiting"}}},
      {"mode",
       {{"type", "string"},
        {"enum", json::array({"lean", "full"})},
        {"description", "lean default; full includes implementation tools"}}},
      {"model",
       {{"type", "string"},
        {"description", ModelPropertyDescription(routes, providers)}}},
      // One object rather than five siblings: each of these needed a sentence
      // saying "optional ceiling for this child", and that sentence is charged
      // to every request that advertises the tool. Grouped, it is said once.
      {"limits",
       {{"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
          {"tool_calls",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
          {"seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
          {"cost", {{"type", "number"}, {"minimum", 0}}},
          {"memory", {{"type", "boolean"}}}}},
        {"description",
         "optional ceilings; omitted fields inherit the defaults, larger "
         "values are clamped, memory=false denies memory"}}}};
  Tool tool = MakeTool(
      "subagent",
      "Delegate an isolated subtask whose compact result saves parent "
      "rounds; for orthogonal parts, one task per part in one batch. spawn "
      "starts a child, followup resumes it, message guides a running child at "
      "its next step or runs a finished one again; the activity tool waits "
      "on, reads or stops it. Name the reusable role and describe it at "
      "spawn, and put what you already found into the prompt so the child "
      "does not look it up again.",
      {{"type", "object"}, {"properties", std::move(properties)}},
      [&api, &routes, &providers, debug, &processes](
          const json& arguments, const ToolContext& context) {
        return RunSubagent(api, processes, routes, providers, debug, arguments,
                           context);
      });
  tool.clamped_arguments = {"limits.steps", "limits.tool_calls",
                            "limits.seconds"};
  // Same reasoning as run, scratch and activity: the per-call budget stops a
  // command running away, and a child the caller is waiting for is neither
  // unsupervised nor unbounded — limits.seconds and the turn bound it, and
  // Escape still returns immediately.
  tool.timeout_s = 0;
  tool.mutating = true;
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.delegates = true;
  tool.retain_output = true;
  tool.available_in_lean = false;
  // Concurrency is enforced by the spawn path (RunShellCommand reserves an
  // activity slot bounded by kMaxBackgroundJobs); this is only a runaway
  // ceiling.
  tool.max_calls_per_turn = kSubagentCallsPerTurn;
  auto describe = [&api, &routes, &providers](const json& arguments) {
    std::string operation = JsonValue(arguments, "operation", "spawn");
    if (operation == "list") return std::string("list agents");
    std::string prompt = JsonValue(arguments, "prompt", "");
    std::string id = JsonValue(arguments, "agent_id", "");
    if (operation == "message") return "[message " + id + "] " + prompt;
    std::string label = RouteSelection(
        ResolveSideRoute(api, routes, providers,
                         NormalizeModelId(JsonValue(arguments, "model", ""))),
        providers);
    const std::string name = JsonValue(arguments, "name", "");
    if (!name.empty()) label = name + " · " + label;
    if (JsonValue(arguments, "mode", "lean") == "full") label += " · full";
    if (JsonValue(arguments, "background", false)) label += " · background";
    if (!id.empty()) label += " · " + id;
    return "[" + label + "] " + prompt;
  };
  // Rows and traces name the work; approval_preview keeps the full form with
  // the route and flags, since that is what a person approves.
  tool.summary = [describe](const json& arguments) {
    const std::string operation = JsonValue(arguments, "operation", "spawn");
    if (operation == "list" || operation == "message") {
      return describe(arguments);
    }
    const std::string name = JsonValue(arguments, "name", "");
    return (name.empty() ? std::string("Subagent") : name) + " · " +
           FirstLine(JsonValue(arguments, "prompt", ""));
  };
  tool.header = [](const json& arguments) {
    const std::string operation = JsonValue(arguments, "operation", "spawn");
    const std::string name = JsonValue(arguments, "name", "");
    const std::string task = FirstLine(JsonValue(arguments, "prompt", ""));
    if (operation == "list") {
      return json{{"verb", {"Listing", "Listed"}}, {"target", "agents"}};
    }
    if (operation == "message") {
      return json{{"verb", {"Messaging", "Messaged"}},
                  {"target", JsonValue(arguments, "agent_id", name)}};
    }
    return json{
        {"verb", operation == "followup" ? json{"Following up", "Followed up"}
                                         : json{"Delegating", "Delegated"}},
        {"target", name.empty() ? task : name + ": " + task}};
  };
  tool.present = [](const json& arguments) {
    json parts = json::array();
    const std::string prompt = JsonValue(arguments, "prompt", "");
    if (!prompt.empty()) parts.push_back(CodePart(prompt, "markdown", "task"));
    for (json& part : GenericInputParts(arguments, {"prompt"})) {
      parts.push_back(std::move(part));
    }
    return parts;
  };
  tool.output_view = "markdown";
  // The summary names the model and the brief; what it cannot show is the
  // authority handed over with them. The child runs with automatic approvals,
  // so approving the spawn approves every tool call that child then decides
  // to make.
  tool.approval_preview = [describe, &api, &processes](const json& arguments) {
    std::string preview = describe(arguments);
    std::string operation = JsonValue(arguments, "operation", "spawn");
    const std::string id = JsonValue(arguments, "agent_id", "");
    // A message to a subagent that has finished runs it again.
    if (operation == "message" && !RunningAgent(processes, id)) {
      operation = "followup";
    }
    if (operation != "spawn" && operation != "followup") return preview;
    // A follow-up runs as the subagent was created unless it says
    // otherwise: what is approved is what will run.
    const json role =
        operation == "followup" && !id.empty() && SafeFileComponent(id) == id
            ? JsonValue(SessionHeader(AgentPath(id)), kSessionHeaderDelegation,
                        json::object())
            : json::object();
    const bool full =
        JsonValue(arguments, "mode", JsonValue(role, "mode", "lean")) == "full";
    preview +=
        "\n\u00b7 the child approves its own tool calls; it writes files and "
        "runs commands unattended, under this session's sandbox";
    preview += std::string("\n\u00b7 toolset ") +
               (full ? "full: reading, editing and running, plus its own "
                       "children"
                     : "lean: reading and running, no file-editing tools");
    const json& limits = ChildLimits(arguments);
    preview +=
        "\n\u00b7 bounded by " +
        std::to_string(JsonValue(limits, "steps", SubagentMaxSteps())) +
        " steps, " +
        std::to_string(
            JsonValue(limits, "tool_calls", SubagentMaxToolCalls())) +
        " tool calls" +
        (api.config.memory_enabled &&
                 JsonValue(limits, "memory", JsonValue(role, "memory", true))
             ? ", memory on"
             : ", memory off");
    return preview;
  };
  return tool;  // Spawns serialize; immediate-background children overlap.
}

}  // namespace uagent
