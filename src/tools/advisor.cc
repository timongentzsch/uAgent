// Copyright 2026 Timon Gentzsch

#include "include/tools/advisor.h"

#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/child_agent.h"
#include "include/tools/shell.h"

namespace uagent {
namespace {

// The advisor answers from the question and the evidence pasted with it, so
// the caller decides what matters instead of the child re-reading the tree.
constexpr size_t kQuestionBytes = 4 * 1024;

std::string AdvisorPrompt(const std::string& question,
                          const std::string& context) {
  std::string prompt =
      "You are an independent advisor to another coding agent. Reason only "
      "from the question and the evidence below; you have no tools and cannot "
      "inspect the workspace. Say plainly what you would do and why, name the "
      "strongest objection to it, and state what you are uncertain about or "
      "what evidence would change your answer. Do not invent file contents or "
      "results. Answer in prose, briefly.\n\n<question>\n" +
      Utf8Trunc(question, kQuestionBytes) + "\n</question>";
  if (!context.empty()) {
    prompt += "\n\n<evidence>\n" +
              Utf8Trunc(context, static_cast<size_t>(AdvisorContextBytes())) +
              "\n</evidence>";
  }
  return prompt;
}

std::string AdvisorTargetLabel(const Api& api,
                               const std::vector<ModelRoute>& routes,
                               const std::vector<NamedProvider>& providers) {
  return RouteSelection(ResolveSideRoute(api, routes, providers,
                                         AdvisorModel()),
                        providers);
}

}  // namespace

std::string AdvisorModel() { return Trim(EnvStr("UAGENT_ADVISOR_MODEL")); }

Tool AdvisorTool(const Api& api, ProcessSupervisor& processes,
                 const std::vector<ModelRoute>& routes,
                 const std::vector<NamedProvider>& providers, bool debug) {
  Tool tool = MakeTool(
      "advisor",
      "Ask a different model for an independent second opinion when a "
      "decision is hard to reverse, a diagnosis resists a genuine attempt, or "
      "two approaches are close. The advisor has no tools and no view of this "
      "conversation: state the question in full and paste the evidence it "
      "needs. Treat the answer as advice to weigh, not instruction.",
      json::parse(R"json({"type":"object","properties":{
          "question":{"type":"string",
            "description":"self-contained question and what you tried"},
          "context":{"type":"string",
            "description":"relevant excerpts: code, diffs, errors, limits"},
          "background":{"type":"boolean",
            "description":"keep working while it thinks; default false"}},
          "required":["question"]})json"),
      [&api, &routes, &providers, debug, &processes](
          const json& arguments, const ToolContext& context) {
        std::string question = Trim(JsonValue(arguments, "question", ""));
        if (question.empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: question is required");
        }
        SideRoute route =
            ResolveSideRoute(api, routes, providers, AdvisorModel());
        if (route.unresolved &&
            route.selection.find('/') != std::string::npos &&
            !CanUseRawModel(api, route.selection)) {
          return ToolFailure(
              ToolErrorCode::kUnavailable,
              "error: unknown advisor route: " + TerminalSafe(route.selection));
        }
        double remaining_budget = 0;
        if (std::optional<ToolResult> blocked =
                ChildAgentBudgetBlock(api, processes, remaining_budget)) {
          return *blocked;
        }
        // No tools and no memory: the advisor is a reasoner, not an agent, and
        // must not write to this session's state. It also carries its own
        // deadline, since a reasoning model runs far longer than a command.
        std::string deadline = std::to_string(AdvisorTimeoutSeconds());
        EnvironmentOverrides environment =
            ChildAgentEnvironment(std::move(route));
        environment.insert(environment.end(),
                           {{"UAGENT_MAX_TURN_SECONDS", deadline},
                            {"UAGENT_REQUEST_TIMEOUT", deadline},
                            {"UAGENT_TOOLSET", "none"},
                            {"UAGENT_MEMORY", "0"},
                            {"UAGENT_MAX_STEPS", "1"},
                            {"UAGENT_MAX_TOOL_CALLS", "0"}});
        if (api.config.session_budget > 0) {
          environment.emplace_back("UAGENT_SESSION_BUDGET",
                                   std::to_string(remaining_budget));
        }
        bool background = JsonValue(arguments, "background", false);
        std::string evidence = Trim(JsonValue(arguments, "context", ""));
        std::string command =
            ChildAgentCommand(debug, AdvisorPrompt(question, evidence));
        return RunShellCommand(processes, context,
                               {.command = std::move(command),
                                .background = background,
                                .immediate = background,
                                .job_kind = "advisor",
                                .environment = std::move(environment)})
            .result;
      });
  // It mutates nothing; approval is asked because the call leaves the machine
  // and costs money, which is how web_search is gated.
  tool.needs_approval = [](const json&) { return true; };
  tool.capabilities = Capability(ToolCapability::kDelegate) |
                      Capability(ToolCapability::kExternal);
  tool.delegates = true;
  tool.retain_output = true;
  tool.available_in_lean = false;
  tool.parallel_safe = true;
  tool.timeout_s = AdvisorTimeoutSeconds();
  tool.max_calls_per_turn = MaxBackgroundJobs();
  tool.summary = [&api, &routes, &providers](const json& arguments) {
    std::string label = AdvisorTargetLabel(api, routes, providers);
    if (JsonValue(arguments, "background", false)) label += " · background";
    return "[" + label + "] " + JsonValue(arguments, "question", "");
  };
  return tool;
}

}  // namespace uagent
