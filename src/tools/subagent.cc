// Copyright 2026 Timon Gentzsch

#include "include/tools/subagent.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/tools/child_agent.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/shell.h"

namespace uagent {
namespace {

constexpr size_t kAdvertisedRoutes = 16;
constexpr int kCollaboratorFormat = 1;

std::string CollaboratorPath(const std::string& id) {
  return UagentDir("collaborators") + "/" + id + ".json";
}

std::string CollaboratorSessionPath(const std::string& id) {
  return UagentDir("collaborators") + "/" + id + ".session.json";
}

std::string NewCollaboratorId() {
  static std::atomic<uint64_t> sequence{0};
  return "agent-" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
         std::to_string(getpid()) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool LoadCollaborator(const std::string& id, json& state, std::string& error) {
  if (id.empty() || SafeFileComponent(id) != id) {
    error = "invalid collaborator id";
    return false;
  }
  std::ifstream input(CollaboratorPath(id));
  if (!input) {
    error = "collaborator not found";
    return false;
  }
  state = json::parse(input, nullptr, false);
  if (state.is_discarded() || !state.is_object() ||
      JsonValue(state, "format", 0) != kCollaboratorFormat ||
      JsonValue(state, "id", "") != id) {
    error = "collaborator record is invalid";
    return false;
  }
  if (JsonValue(state, "cwd", "") != CanonicalCwd()) {
    error = "collaborator belongs to a different workspace";
    return false;
  }
  return true;
}

ToolResult SaveCollaborator(const json& state) {
  return ToolAtomicWrite(CollaboratorPath(JsonValue(state, "id", "")),
                         JsonDump(state, 2) + "\n", kPrivateFileMode,
                         /*preserve_mode=*/true);
}

std::optional<int64_t> ActiveCollaborator(const ProcessSupervisor& processes,
                                          const std::string& id) {
  for (const BgJob& job : processes.Snapshot()) {
    if (job.source_id == id) return ActivityId(job);
  }
  return std::nullopt;
}

ToolResult ListCollaborators(const ProcessSupervisor& processes) {
  namespace fs = std::filesystem;
  std::error_code error;
  std::vector<json> records;
  for (fs::directory_iterator it(UagentDir("collaborators"), error), end;
       !error && it != end && records.size() < 100; it.increment(error)) {
    if (!it->is_regular_file(error) ||
        !it->path().filename().string().ends_with(".json") ||
        it->path().filename().string().ends_with(".session.json")) {
      continue;
    }
    std::ifstream input(it->path());
    json state = json::parse(input, nullptr, false);
    if (state.is_discarded() || !state.is_object() ||
        JsonValue(state, "cwd", "") != CanonicalCwd()) {
      continue;
    }
    std::string id = JsonValue(state, "id", "");
    json row = {
        {"id", id},
        {"model", JsonValue(state, "model", "")},
        {"mode", JsonValue(state, "mode", "lean")},
        {"status", ActiveCollaborator(processes, id) ? "running" : "idle"}};
    records.push_back(std::move(row));
  }
  return ToolSuccess(records.empty() ? "no collaborators"
                                     : JsonDump(records, 2));
}

// Concurrency is enforced by the spawn path (RunShellCommand reserves an
// activity slot bounded by MaxBackgroundJobs); this is only a runaway ceiling.
int64_t MaxSubagentCallsPerTurn() { return SubagentCallsPerTurn(); }

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

std::string ModelPropertyDescription(
    const std::vector<ModelRoute>& routes,
    const std::vector<NamedProvider>& providers) {
  std::vector<std::string> aliases;
  aliases.reserve(routes.size());
  for (const ModelRoute& route : routes) aliases.push_back(route.name);

  std::vector<std::string> prefixes;
  prefixes.reserve(providers.size());
  for (const NamedProvider& provider : providers) {
    prefixes.push_back(provider.name + "/MODEL");
  }

  // Naming an alias here is an override, not the default. Spelling that out
  // matters: a child sent to an unreachable alias fails outright rather than
  // falling back, so the default must read as the safe choice.
  std::string description =
      "Child model route. Omit to inherit the delegated default in runtime "
      "context; name one only to override it for this subtask.";
  std::string configured = JoinSelections(std::move(aliases));
  if (!configured.empty()) {
    description += " Overrides: " + configured + ".";
  }
  std::string dynamic = JoinSelections(std::move(prefixes));
  if (!dynamic.empty()) {
    description += " Or any model on a named provider: " + dynamic + ".";
  }
  return description;
}

// A delegated child runs on the parent's route unless the request or
// UAGENT_SUBAGENT_MODEL names one; an empty selection tells the shared
// resolver to inherit.
SideRoute ResolveSubagentRoute(const Api& api,
                               const std::vector<ModelRoute>& routes,
                               const std::vector<NamedProvider>& providers,
                               const std::string& requested) {
  return ResolveSideRoute(
      api, routes, providers,
      requested.empty() ? NormalizeModelId(SubagentModel()) : requested);
}

std::string SubagentTargetLabel(const Api& api,
                                const std::vector<ModelRoute>& routes,
                                const std::vector<NamedProvider>& providers,
                                const std::string& requested) {
  return RouteSelection(ResolveSubagentRoute(api, routes, providers, requested),
                        providers);
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

}  // namespace

std::string DefaultSubagentModel(const Api& api) {
  std::string selection = NormalizeModelId(SubagentModel());
  if (!selection.empty()) return selection;
  return api.model;
}

std::string DelegationRuntimeContext(const Api& api) {
  // No provider list reaches here; the built-in templates still scope the
  // common routes, and a custom endpoint degrades to a bare model id.
  std::string parent = TerminalSafe(RouteSelection(api, {}));
  std::string child_model = DefaultSubagentModel(api);
  if (child_model == api.model) {
    return "[delegation: parent=" + parent + "; default=parent]";
  }
  return "[delegation: parent=" + parent +
         "; default=" + TerminalSafe(child_model) + "]";
}

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug) {
  json properties = {
      {"operation",
       {{"type", "string"},
        {"enum", json::array({"spawn", "followup", "message", "list"})},
        {"description",
         "spawn default; followup resumes a durable child; "
         "message queues guidance; list shows collaborators"}}},
      {"agent_id",
       {{"type", "string"},
        {"description", "durable collaborator id for followup or message"}}},
      {"prompt",
       {{"type", "string"},
        {"description",
         "standalone brief for spawn; next message for a "
         "followup or queued message"}}},
      {"directive",
       {{"type", "string"},
        {"description",
         "persistent coordinator guidance prepended to followups; an "
         "explicit empty string clears it"}}},
      {"background",
       {{"type", "boolean"},
        {"description",
         "default true; false blocks and returns the final result directly"}}},
      {"mode",
       {{"type", "string"},
        {"enum", json::array({"lean", "full"})},
        {"description", "lean default; full includes implementation tools"}}},
      {"model",
       {{"type", "string"},
        {"description", ModelPropertyDescription(routes, providers)}}},
      {"max_steps",
       {{"type", "integer"},
        {"minimum", 1},
        {"maximum", 500},
        {"description",
         "optional model-round ceiling for this child; omit for the "
         "configured default"}}},
      {"max_tool_calls",
       {{"type", "integer"},
        {"minimum", 1},
        {"maximum", 500},
        {"description", "optional tool-call ceiling for this child"}}},
      {"max_seconds",
       {{"type", "integer"},
        {"minimum", 1},
        {"maximum", 3600},
        {"description", "wall-clock ceiling for a foreground child"}}},
      {"max_cost",
       {{"type", "number"},
        {"minimum", 0},
        {"description", "cost ceiling; clamped to the session's remainder"}}},
      {"memory",
       {{"type", "boolean"},
        {"description", "false denies the child memory; default inherits"}}}};
  Tool tool = MakeTool(
      "subagent",
      "Delegate an isolated subtask whose compact result avoids multiple "
      "parent rounds; for a broad request with orthogonal parts, issue one "
      "task per part in a single batch. Spawn creates a durable collaborator "
      "whose conversation can be resumed with operation=followup; message "
      "queues guidance for its next followup, while activity handles waiting, "
      "output and stopping. Keep background=true when useful parent work can "
      "continue.",
      {{"type", "object"}, {"properties", std::move(properties)}},
      [&api, &routes, &providers, debug, &processes](
          const json& arguments, const ToolContext& context) {
        std::string operation = JsonValue(arguments, "operation", "spawn");
        if (operation == "list") return ListCollaborators(processes);
        if (operation != "spawn" && operation != "followup" &&
            operation != "message") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: operation must be spawn, followup, "
                             "message, or list");
        }

        std::string collaborator_id = JsonValue(arguments, "agent_id", "");
        json collaborator;
        if (operation == "spawn") {
          if (!collaborator_id.empty()) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: agent_id is assigned by spawn");
          }
          collaborator_id = NewCollaboratorId();
          collaborator = {
              {"format", kCollaboratorFormat},
              {"id", collaborator_id},
              {"cwd", CanonicalCwd()},
              {"session_file", CollaboratorSessionPath(collaborator_id)},
              {"created_at", UtcStamp()},
              {"directive", JsonValue(arguments, "directive", "")},
              {"mailbox", json::array()}};
        } else {
          std::string load_error;
          if (!LoadCollaborator(collaborator_id, collaborator, load_error)) {
            return ToolFailure(ToolErrorCode::kNotFound,
                               "error: " + load_error);
          }
        }

        std::string prompt = JsonValue(arguments, "prompt", "");
        if (operation == "message") {
          if (arguments.contains("directive")) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: message cannot change directive; use "
                               "followup");
          }
          if (prompt.empty()) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: message requires prompt");
          }
          if (!collaborator.contains("mailbox") ||
              !collaborator["mailbox"].is_array()) {
            collaborator["mailbox"] = json::array();
          }
          collaborator["mailbox"].push_back(prompt);
          collaborator["updated_at"] = UtcStamp();
          ToolResult saved = SaveCollaborator(collaborator);
          if (!saved.Ok()) return saved;
          return ToolSuccess("queued message for collaborator " +
                             collaborator_id);
        }

        if (std::optional<int64_t> active =
                ActiveCollaborator(processes, collaborator_id)) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: collaborator " + collaborator_id +
                                 " is already running as activity " +
                                 std::to_string(*active));
        }
        if (operation == "followup") {
          if (arguments.contains("directive")) {
            collaborator["directive"] = JsonValue(arguments, "directive", "");
          }
          std::string directive = JsonValue(collaborator, "directive", "");
          if (!directive.empty()) {
            prompt = "[collaborator directive]\n" + directive +
                     (prompt.empty() ? "" : "\n\n" + prompt);
          }
        }
        if (operation == "followup" && collaborator.contains("mailbox") &&
            collaborator["mailbox"].is_array()) {
          for (const json& queued : collaborator["mailbox"]) {
            if (!queued.is_string()) continue;
            if (!prompt.empty()) prompt += "\n\n";
            prompt += "[queued guidance]\n" + queued.get<std::string>();
          }
        }
        if (prompt.empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: spawn or followup requires prompt or "
                             "queued guidance");
        }

        std::string mode = JsonValue(arguments, "mode",
                                     JsonValue(collaborator, "mode", "lean"));
        if (mode != "lean" && mode != "full") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: mode must be lean or full");
        }
        const std::string requested = NormalizeModelId(JsonValue(
            arguments, "model", JsonValue(collaborator, "model", "")));
        SideRoute route =
            ResolveSubagentRoute(api, routes, providers, requested);
        std::string route_label = SubagentDiagnosticRoute(route, providers);
        if (route.unresolved &&
            route.selection.find('/') != std::string::npos &&
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
        EnvironmentOverrides environment =
            ChildAgentEnvironment(std::move(route));
        // A caller that knows the shape of the subtask may raise or lower the
        // ceiling for that one child; the schema bounds it, and the session
        // cost budget still applies underneath.
        int64_t steps = JsonValue(arguments, "max_steps", SubagentMaxSteps());
        int64_t tool_calls =
            JsonValue(arguments, "max_tool_calls", SubagentMaxToolCalls());
        bool background = JsonValue(arguments, "background", true);
        // A caller may deny memory but not grant it: the session decides what
        // this process may read, and a child cannot widen that.
        bool child_memory = api.config.memory_enabled &&
                            JsonValue(arguments, "memory",
                                      JsonValue(collaborator, "memory", true));
        environment.insert(
            environment.end(),
            {{"UAGENT_MAX_STEPS", std::to_string(steps)},
             {"UAGENT_MAX_TOOL_CALLS", std::to_string(tool_calls)},
             {"UAGENT_TOOLSET", std::move(mode)},
             {"UAGENT_MEMORY", child_memory ? "1" : "0"},
             {"UAGENT_INTERNAL_SESSION_FILE",
              JsonValue(collaborator, "session_file", "")},
             // The parent brief is standalone. Re-inlining every always-on
             // memory in each child only duplicates context and emits a
             // misleading truncation warning when that optional cache is full.
             {"UAGENT_MEMORY_ALWAYS_BYTES", "0"}});
        // Only a background child is polled while it runs. A foreground child
        // is read once, where progress lines would only pad the answer the
        // parent quotes.
        if (background) {
          environment.emplace_back("UAGENT_HEADLESS_PROGRESS", "1");
        }
        // Tightening is the caller's to do; loosening is not. A requested
        // budget above what the session has left is clamped, and the clamp is
        // reported rather than applied behind the caller's back.
        std::vector<std::string> clamped;
        double child_budget = JsonValue(arguments, "max_cost", 0.0);
        if (api.config.session_budget > 0) {
          if (child_budget <= 0 || child_budget > remaining_budget) {
            if (child_budget > remaining_budget) {
              clamped.push_back("max_cost to " + FmtCost(remaining_budget) +
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
        // The per-call budget bounds a command that might run away. A child
        // the caller chose to wait for is supervised, so it is bounded by
        // max_seconds when given and by the turn otherwise.
        ToolContext child_context = context;
        int64_t max_seconds = JsonValue(arguments, "max_seconds", int64_t{0});
        int64_t ceiling = SubagentTimeoutSeconds();
        if (ceiling > 0 && (max_seconds <= 0 || max_seconds > ceiling)) {
          if (max_seconds > ceiling) {
            clamped.push_back("max_seconds to " + std::to_string(ceiling) +
                              ", this build's ceiling");
          }
          max_seconds = ceiling;
        }
        if (max_seconds > 0) child_context = context.WithTimeout(max_seconds);
        std::string command = ChildAgentCommand(debug, prompt);
        ShellCommandResult child =
            RunShellCommand(processes, child_context,
                            {.command = std::move(command),
                             .background = background,
                             .immediate = background,
                             .job_kind = "subagent",
                             .activity_label = route_label,
                             .source_id = collaborator_id,
                             .completion_notes = clamped,
                             .environment = std::move(environment)});
        const bool launched = child.launched;
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
                  : std::move(result.output) +
                        ChildAgentConstraintNotes(clamped);
        }
        if (!result.Ok() && result.status != CompletionStatus::kCancelled) {
          ChildAgentFailureStage stage =
              child.wait_status ? ChildAgentFailureStage::kExecution
                                : ChildAgentFailureStage::kSpawn;
          result.output =
              ChildAgentFailureReport(route_label, stage, result.output) +
              ChildAgentConstraintNotes(clamped);
        }
        if (launched) {
          collaborator["mode"] = JsonValue(
              arguments, "mode", JsonValue(collaborator, "mode", "lean"));
          collaborator["model"] = requested;
          collaborator["memory"] = child_memory;
          collaborator["mailbox"] = json::array();
          collaborator["updated_at"] = UtcStamp();
          ToolResult saved = SaveCollaborator(collaborator);
          if (saved.Ok()) {
            result.output += "\n[collaborator " + collaborator_id +
                             "; resume with subagent operation=followup]";
          } else {
            result.output +=
                "\n[warning: collaborator metadata was not saved: " +
                TerminalSafe(saved.output) + "]";
          }
        }
        return result;
      });
  tool.clamped_arguments = {"max_steps", "max_tool_calls", "max_seconds"};
  // Same reasoning as run, scratch and activity: the per-call budget stops a
  // command running away, and a child the caller is waiting for is neither
  // unsupervised nor unbounded — max_seconds and the turn bound it, and
  // Escape still returns immediately.
  tool.timeout_s = 0;
  tool.mutating = true;
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.delegates = true;
  tool.retain_output = true;
  tool.available_in_lean = false;
  tool.max_calls_per_turn = MaxSubagentCallsPerTurn();
  tool.summary = [&api, &routes, &providers](const json& arguments) {
    std::string operation = JsonValue(arguments, "operation", "spawn");
    if (operation == "list") return std::string("list collaborators");
    std::string mode = JsonValue(arguments, "mode", "lean");
    std::string prompt = JsonValue(arguments, "prompt", "");
    std::string id = JsonValue(arguments, "agent_id", "");
    if (operation == "message") return "[message " + id + "] " + prompt;
    std::string label = SubagentTargetLabel(
        api, routes, providers,
        NormalizeModelId(JsonValue(arguments, "model", "")));
    if (mode == "full") label += " · full";
    if (!JsonValue(arguments, "background", true)) label += " · foreground";
    if (!id.empty()) label += " · " + id;
    return "[" + label + "] " + prompt;
  };
  return tool;  // Spawns serialize; immediate-background children overlap.
}

}  // namespace uagent
