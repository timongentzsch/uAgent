// Copyright 2026 Timon Gentzsch

#include "include/app/bootstrap.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/prompt.h"
#include "include/api.h"
#include "include/app/reference.h"
#include "include/browser/browser.h"
#include "include/cli.h"
#include "include/core/config.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/skills.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp/discover.h"
#include "include/mcp/register.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/tools/adapt_system.h"
#include "include/tools/browser.h"
#include "include/tools/configure.h"
#include "include/tools/memory.h"
#include "include/tools/registry.h"
#include "include/tools/session.h"
#include "include/tools/skill.h"
#include "include/tools/subagent.h"
#include "include/tools/web_fetch.h"
#include "include/tools/web_search.h"
#include "include/ui/display.h"
#include "include/ui/presentation.h"

namespace uagent {
namespace {

BootstrapResult Failure(std::string error, int exit_code = 1) {
  return {nullptr, std::move(error), exit_code};
}

class ScopedChannelInput {
 public:
  explicit ScopedChannelInput(ApplicationChannel* channel)
      : active_(channel != nullptr) {
    if (!channel) return;
    SetInteractiveReadHandler(
        [channel](const InteractionRequest& request, bool* eof) {
          return channel->ReadInteraction(request, eof);
        });
  }

  ~ScopedChannelInput() {
    if (active_) SetInteractiveReadHandler({});
  }

  void Transfer() { active_ = false; }

 private:
  bool active_;
};

void PrintWarning(const std::string& warning) {
  if (!warning.empty()) {
    Emit(NoticeEvent(PresentationStatus::kWarned, warning));
  }
}

// A y/N prompt. Silence and EOF both decline: an unattended run must never
// grant trust or approval by accident.
bool Confirm(InteractionRequest request) {
  bool cancelled = false;
  bool eof = false;
  std::string answer = ReadChoiceLine(std::move(request), cancelled, eof);
  return !cancelled && !eof &&
         (answer == "y" || answer == "Y" || answer == "yes");
}

// A mandatory-human decision needs a real person on the other end. Headless
// runs, delegated children and piped input cannot supply one.
bool InteractiveApprovalAvailable() {
  return (isatty(STDIN_FILENO) || InteractiveReadAvailable()) &&
         AgentDepth() == 0;
}

bool ResolveProjectTrust(const Options& options, bool& trusted,
                         json& trusted_snapshot, std::string& error,
                         int& exit_code) {
  trusted = options.trust_project || TrustProjectConfig();
  if (!trusted) trusted = ProjectConfigTrusted(&trusted_snapshot);
  bool mcp_present = ProjectMcpPresent();
  bool agent_config_present = ProjectAgentConfigPresent();
  if ((mcp_present || agent_config_present) && !trusted) {
    std::string surfaces =
        mcp_present ? (agent_config_present ? ".mcp.json and .uagent/.config"
                                            : ".mcp.json")
                    : ".uagent/.config";
    if (!InteractiveApprovalAvailable() || !options.prompt.empty()) {
      if (mcp_present) {
        error =
            "project .mcp.json is untrusted; rerun with "
            "--trust-project-config after reviewing it";
        exit_code = 2;
        return false;
      }
      fprintf(stderr,
              "project .uagent/.config is untrusted and was ignored; rerun "
              "with --trust-project-config after reviewing it\n");
    } else {
      trusted = Confirm(
          {.kind = "project.trust",
           .prompt = "Trust this workspace's " + surfaces + "? [y/N] ",
           .options = json::array({{{"value", "y"}, {"label", "Trust"}},
                                   {{"value", "n"}, {"label", "Decline"}}})});
      if (trusted && !TrustProjectConfig(error, &trusted_snapshot)) {
        error = "cannot save project trust: " + error;
        return false;
      }
    }
  }
  if (mcp_present && trusted && trusted_snapshot.is_null()) {
    if (!ProjectMcpSnapshot(trusted_snapshot, error)) {
      error = "cannot load trusted project config: " + error;
      return false;
    }
  }
  return true;
}

// Project docs and memory context share one byte budget: what the project
// instructions do not spend is what the memory index may.
ProjectInstructions LoadInstructions(const std::filesystem::path& workspace,
                                     const RuntimeConfig& config,
                                     bool memory_child, size_t project_limit) {
  ProjectInstructions instructions;
  if (!memory_child) {
    instructions = LoadProjectInstructions(workspace, project_limit);
  }
  if (config.memory_enabled) {
    size_t remaining = instructions.text.size() >= project_limit
                           ? 0
                           : project_limit - instructions.text.size();
    MemoryIndex memories = LoadMemoryIndex(workspace, remaining);
    instructions.memory_index = std::move(memories.text);
    instructions.memory_sources = std::move(memories.sources);
    instructions.memory_truncated |= memories.truncated;
    instructions.memory_limit = remaining;

    size_t always_bytes =
        static_cast<size_t>(std::max(int64_t{0}, config.memory_always_bytes));
    if (always_bytes > 0) {
      MemoryIndex always = LoadAlwaysOnMemory(workspace, always_bytes);
      instructions.memory_always = std::move(always.text);
      for (const std::string& source : always.sources) {
        if (std::find(instructions.memory_sources.begin(),
                      instructions.memory_sources.end(),
                      source) == instructions.memory_sources.end()) {
          instructions.memory_sources.push_back(source);
        }
      }
      if (always.truncated) {
        instructions.memory_truncated = true;
        instructions.memory_limit = always_bytes;
      }
    }
  }
  return instructions;
}

std::vector<Tool> BuildTools(AppContext& context,
                             const std::filesystem::path& workspace,
                             const json& trusted_snapshot,
                             std::vector<Skill> skills, std::string& error) {
  Api& api = context.runtime.api;
  AppRuntime& runtime = context.runtime;
  // One read of the toolset selector: the three shapes it can take are one
  // decision, not three unrelated conditions.
  const std::string toolset = EnvStr("UAGENT_TOOLSET");
  std::vector<Tool> tools = BuiltinTools(
      runtime.processes, workspace,
      AdaptiveSystemEnabled() ? &runtime.adaptive_system : nullptr);
  if (!runtime.config.memory_enabled) {
    std::erase_if(tools, [](const Tool& tool) { return tool.memory_store; });
  }
  if (toolset == "memory") {
    std::erase_if(tools, [](const Tool& tool) { return !tool.memory_store; });
    return tools;
  }
  // A tool-less child remains useful for constrained internal tasks.
  if (toolset == "none") return {};
  ConfigProposalFactory prepare;
  if (InteractiveApprovalAvailable() &&
      (context.tool_policy.allowed & Capability(ToolCapability::kMutate))) {
    prepare = [app = &context](ConfigProposalScope scope,
                               const std::vector<ConfigChange>& changes) {
      return PrepareConfigProposal(scope, changes, app->config_manager,
                                   app->config_manager.ProjectTrusted());
    };
  }
  tools.push_back(UagentTool(
      [app = &context](SelfTopic topic, const std::string& name) {
        return DescribeSelf(
            topic, name,
            SelfDescriptionInputs{app->config_manager, app->runtime.config,
                                  app->runtime.api, app->tools,
                                  app->agent.get()});
      },
      prepare, std::make_shared<ConfigApprovals>()));
  WebSearchRoute search_route =
      SelectWebSearchRoute(api, context.provider.providers);
  if (search_route.Valid()) {
    tools.push_back(
        WebSearchTool(api, runtime.side_usage, context.provider.providers));
  }
  // Reading a named URL needs no hosted route, so it does not follow search's
  // availability.
  tools.push_back(WebFetchTool(api));
  if (!browser::DataDirectory().empty() && context.options.browser_session &&
      context.channel && !context.channel->SessionPath().empty() &&
      AgentDepth() == 0) {
    tools.push_back(BrowserTool(HashHex(context.channel->SessionPath())));
  }
  // The default lean child is an isolation and context-efficiency boundary:
  // do not clone the parent's entire MCP fleet into every delegation. A root
  // lean session and an explicitly requested full child still get MCP.
  if (AgentDepth() == 0 || toolset != "lean") {
    error = McpRegister(tools, runtime.mcp, runtime.config, trusted_snapshot);
    if (!error.empty()) return {};
  }
  if (CanDelegate()) {
    tools.push_back(SubagentTool(api, runtime.processes,
                                 context.provider.routes,
                                 context.provider.providers,
                                 context.options.debug, &runtime.collaborator));
  }
  // Peer sessions are text-only and isolation-gated by links, so the session
  // tool is safe in every toolset, lean included.
  tools.push_back(SessionTool());
  if (toolset == "lean") {
    KeepLeanTools(tools);
  }
  ApplyToolPolicy(tools, context.tool_policy);
  std::vector<std::string> tool_names = ToolNames(tools);
  KeepSupportedSkills(skills, tool_names);
  if (!skills.empty()) {
    std::vector<Tool> skill_tool{SkillTool(skills, tool_names)};
    ApplyToolPolicy(skill_tool, context.tool_policy);
    if (!skill_tool.empty()) {
      tool_names.push_back(skill_tool.front().name);
      tools.push_back(std::move(skill_tool.front()));
    }
  }
  return tools;
}

// Approval policy in one place, so the prompt and the yolo shortcut cannot
// drift apart from the debug record of what was granted. A mandatory-human
// call ignores every automatic-approval switch and denies when no human can
// answer. That is defense in depth for the built-in file tools, not a
// boundary: until commands are sandboxed an approved shell reaches the same
// paths with none of these checks in front of it.
Agent::Approver MakeApprover(AppContext* app) {
  return [app](const Tool& tool, const json& arguments, int64_t turn) {
    static std::atomic<uint64_t> sequence{0};
    ApprovalClass required = RequiredApproval(tool, arguments);
    bool mandatory = required == ApprovalClass::kMandatoryHuman;
    std::string key = PermissionKey(tool, arguments, required);
    const std::string root = CanonicalCwd();
    bool session_rule = !mandatory && app->session_approvals.contains(key);
    bool repository_rule =
        !mandatory && !session_rule && RepositoryPermissionAllows(root, key);
    bool automatic =
        !mandatory && (ApprovalIsYolo() || session_rule || repository_rule);
    bool granted = true;
    json review = json::object();
    std::string request_id =
        "approval-" + std::to_string(sequence.fetch_add(1) + 1);
    const std::string raw_payload = tool.approval_preview
                                        ? tool.approval_preview(arguments)
                                        : ToolSummary(tool, arguments);
    if (!automatic && !mandatory &&
        CurrentApprovalMode() == ApprovalMode::kAuto) {
      AutoPermissionReview decision =
          ReviewPermission(app->runtime.permission_api, app->runtime.config,
                           app->runtime.side_usage, turn, tool, raw_payload,
                           app->agent->History().LastText(MessageKind::kUser));
      const char* choice =
          decision.decision == AutoPermissionDecision::kAllow  ? "allow"
          : decision.decision == AutoPermissionDecision::kDeny ? "deny"
                                                               : "ask";
      review = {{"choice", choice},
                {"probabilities", std::move(decision.probabilities)}};
      if (!decision.error.empty()) review["error"] = decision.error;
      if (decision.decision == AutoPermissionDecision::kAllow) {
        automatic = true;
        granted = true;
      } else if (decision.decision == AutoPermissionDecision::kDeny) {
        automatic = true;
        granted = false;
      } else if (!InteractiveApprovalAvailable()) {
        automatic = true;
        granted = false;
      }
      DebugLog("permission_review", {{"tool", tool.name}, {"result", review}});
    }
    if (!automatic) {
      // Print the full command/payload before asking, so long commands are
      // never truncated in the approval prompt. A tool with more to show than
      // its one-line label supplies its own preview.
      std::string payload = TerminalSafe(raw_payload);
      // Reaching µAgent's own configuration is the reason a tool escalates by
      // its path; a tool that escalates for its own reason names it.
      std::string reason = tool.mandatory_reason.empty()
                               ? "changes \u00b5Agent's own configuration"
                               : tool.mandatory_reason;
      Emit(Event{
          EventId::kApprovalRequested,
          {{"id", request_id},
           {"tool", tool.name},
           {"preview", payload},
           {"scope", PermissionScope()},
           {"mandatory_human", mandatory},
           {"mandatory_reason", mandatory ? reason : std::string()},
           {"choices", mandatory ? json::array({"yes", "no"})
                                 : json::array({"once", "session", "repository",
                                                "no", "guidance"})}}});
      if (!app->channel) {
        std::string headline = "allow " + TerminalSafe(tool.name) + RST();
        if (mandatory) headline += " \u2014 " + reason;
        std::string styled_payload = ColorizeDiffLines(payload);
        fprintf(stdout, "%s%s\n%s\n%s\n", YEL(), headline.c_str(),
                styled_payload.c_str(), RST());
      }
      if (mandatory && !InteractiveApprovalAvailable()) {
        fprintf(stdout,
                "%s\u00b7 denied: this change needs a person, and no "
                "interactive terminal is attached%s\n",
                RED(), RST());
        granted = false;
      } else if (mandatory) {
        std::string question = std::string(YEL()) + "allow " +
                               TerminalSafe(tool.name) + "? [y/N] " + RST();
        granted = Confirm(
            {.id = request_id,
             .kind = "approval",
             .prompt = std::move(question),
             .options = json::array({{{"value", "y"}, {"label", "Allow"}},
                                     {{"value", "n"}, {"label", "Deny"}}})});
      } else {
        // Anything else is guidance: denied, and queued as steering.
        std::string question =
            std::string(YEL()) + "allow " + TerminalSafe(tool.name) +
            "? [y] once  [s] session  [a] repository  [n] no — or say what "
            "to do instead: " +
            RST();
        bool cancelled = false;
        bool eof = false;
        std::string answer = Trim(ReadChoiceLine(
            {.id = request_id,
             .kind = "approval",
             .prompt = std::move(question),
             .options = json::array(
                 {{{"value", "y"}, {"label", "Allow once"}},
                  {{"value", "s"}, {"label", "Allow for this session"}},
                  {{"value", "a"}, {"label", "Always in this repository"}},
                  {{"value", "n"}, {"label", "Deny"}},
                  {{"value", "guidance"}, {"label", "Send guidance"}}})},
            cancelled, eof));
        std::string choice = AsciiLower(answer);
        bool remember_session = choice == "s" || choice == "session";
        bool remember_repository =
            choice == "a" || choice == "always" || choice == "repository";
        granted = !cancelled && !eof &&
                  (choice == "y" || choice == "yes" || remember_session ||
                   remember_repository);
        if (granted && remember_session) app->session_approvals.insert(key);
        if (granted && remember_repository) {
          std::string error;
          if (!RememberRepositoryPermission(root, key, tool.name, raw_payload,
                                            error)) {
            Emit(NoticeEvent(
                PresentationStatus::kWarned,
                "· allowed once; could not save permission rule: " + error));
          }
        }
        if (!granted && !cancelled && !eof && !answer.empty() &&
            choice != "n" && choice != "no") {
          SteeringState().Queue(answer);
        }
      }
    }
    DebugLog("approval", {{"tool", tool.name},
                          {"automatic", automatic},
                          {"mode", ApprovalModeName(CurrentApprovalMode())},
                          {"session_rule", session_rule},
                          {"repository_rule", repository_rule},
                          {"review", review},
                          {"mandatory_human", mandatory},
                          {"granted", granted}});
    Emit(Event{EventId::kApprovalResolved,
               {{"id", request_id},
                {"tool", tool.name},
                {"automatic", automatic},
                {"mode", ApprovalModeName(CurrentApprovalMode())},
                {"review", review},
                {"mandatory_human", mandatory},
                {"granted", granted}}});
    return granted;
  };
}

// An MCP rescan can add or drop tools mid-session, so the policy filter has to
// run again on whatever the refresh produced.
Agent::ToolRefresher MakeToolRefresher(AppContext* app) {
  return [app](std::chrono::steady_clock::time_point deadline) {
    bool changed = McpRefreshTools(app->tools, app->runtime.mcp,
                                   app->runtime.config, deadline);
    if (changed) {
      ApplyToolPolicy(app->tools, app->tool_policy);
      app->session_approvals.clear();
    }
    return changed;
  };
}

// Two things a session must not discover only when a command fails: that the
// sandbox it asked for is not running, and that a root it listed was dropped.
void ReportSandbox() {
  if (ApprovalIsYolo()) return;
  const SandboxStatus& status = SandboxRuntime();
  if (status.mode == SandboxMode::kDegraded) {
    Emit(Event{EventId::kCapabilityChanged,
               {{"feature", "sandbox"},
                {"from", true},
                {"to", false},
                {"reason", status.reason}}});
    Emit(NoticeEvent(
        PresentationStatus::kWarned,
        "\u00b7 sandbox: " + status.reason + "; commands run unconfined"));
  }
  if (status.rejected.empty()) return;
  std::string dropped;
  for (const std::string& root : status.rejected) {
    dropped += (dropped.empty() ? "" : ", ") + root;
  }
  Emit(NoticeEvent(PresentationStatus::kWarned,
                   "\u00b7 sandbox: not granted as writable: " + dropped));
}

void LogReady(const AppContext& context) {
  const Api& api = context.runtime.api;
  const RuntimeConfig& config = context.runtime.config;
  const std::string toolset = LeanToolset() ? "lean" : "full";
  const std::string run_mode =
      context.channel
          ? "channel"
          : (context.options.prompt.empty() ? "interactive" : "headless");
  std::string overlay_digest;
  (void)PromptOverlay(&overlay_digest);
  json provenance = BuildProvenanceJson();
  provenance["toolset"] = toolset;
  provenance["sandbox"] = SandboxDiagnosticJson();
  provenance["active_schema_digest"] =
      HashHex(JsonDump(ToolSchemas(context.tools)));
  provenance["behavior"] = {
      {"reasoning_effort", api.reasoning_effort},
      {"openrouter_variant", config.openrouter_variant},
      {"context_window", api.ctx_window},
      {"memory", config.memory_enabled},
      {"memory_generate", config.memory_generate},
      {"run_mode", run_mode},
      {"approval", ApprovalModeName(CurrentApprovalMode())},
      {"auto_compact_pct", AutoCompactPct()},
      {"auto_compact_tokens", AutoCompactTokens()},
      {"tool_concurrency", ToolConcurrency()},
      {"tool_result_chars", ToolResultCap()},
      {"tool_batch_result_chars", ToolBatchResultCap()},
      {"steering", SteeringEnabled()},
      {"adaptive_system", AdaptiveSystemEnabled()},
      {"max_tokens", MaxOutputTokens()},
      {"prompt_overlay",
       overlay_digest.empty() ? json(nullptr) : json(overlay_digest)},
  };
  Emit(Event{
      EventId::kSessionReady,
      {{"base_url", RedactedUrl(api.base_url)},
       {"model", api.RequestModel()},
       {"route", RouteSelection(api, context.provider.providers)},
       {"provenance", std::move(provenance)},
       {"reasoning_effort", api.reasoning_effort},
       {"openrouter_variant", config.openrouter_variant},
       {"capabilities", api.capabilities.DiagnosticJson()},
       {"configured_models", context.provider.routes.size()},
       {"context_window", api.ctx_window},
       {"tools", context.tools.size()},
       {"toolset", toolset},
       {"memory", config.memory_enabled},
       {"memory_generate", config.memory_generate},
       {"run_mode", run_mode},
       {"output_mode", context.options.json_stream
                           ? "json-stream"
                           : (context.options.json ? "json" : "text")},
       {"yolo", ApprovalIsYolo()},
       {"auto_compact_pct", AutoCompactPct()},
       {"auto_compact_tokens", AutoCompactTokens()},
       {"openrouter_provider", config.openrouter_provider},
       {"openrouter_fallbacks", config.openrouter_fallbacks},
       {"tool_concurrency", ToolConcurrency()},
       {"tool_result_chars", ToolResultCap()},
       {"tool_batch_result_chars", ToolBatchResultCap()},
       {"attachment_mb", AttachmentLimitMb()},
       {"image_detail", ImageDetail()},
       {"steering", SteeringEnabled()},
       {"adaptive_system", AdaptiveSystemEnabled()},
       {"max_tokens", MaxOutputTokens()},
       {"limits", config.DiagnosticJson()},
       {"effective_config", context.config_manager.DiagnosticJson(config)}}});
  ReportSandbox();
}

}  // namespace

HeadlessOutput::~HeadlessOutput() { Restore(); }

bool HeadlessOutput::Silence() {
  if (saved_stdout_) return true;
  fflush(stdout);
  saved_stdout_ = Fd(dup(STDOUT_FILENO));
  if (!saved_stdout_) return false;
  fcntl(saved_stdout_.Get(), F_SETFD, FD_CLOEXEC);
  Fd null_fd(open("/dev/null", O_WRONLY));
  if (!null_fd || dup2(null_fd.Get(), STDOUT_FILENO) < 0) {
    Restore();
    return false;
  }
  return true;
}

void HeadlessOutput::Restore() {
  if (!saved_stdout_) return;
  fflush(stdout);
  dup2(saved_stdout_.Get(), STDOUT_FILENO);
  saved_stdout_.Reset();
}

AppContext::AppContext(RuntimeConfig config, ConfigManager manager,
                       Options parsed_options, Observability& observation_sink,
                       ApplicationChannel* application_channel)
    : config_manager(std::move(manager)),
      runtime(std::move(config)),
      observability(observation_sink),
      channel(application_channel),
      options(std::move(parsed_options)) {}

AppContext::~AppContext() {
  if (channel) SetInteractiveReadHandler({});
}

BootstrapResult Bootstrap(Options options, const char* executable,
                          Observability& observability,
                          ApplicationChannel* channel) {
  ScopedChannelInput channel_input(channel);
  if (channel) observability.EnableTerminal(false);
  SetExecutablePath(executable);
  // Nothing chdir()s during startup, so the canonical workspace is invariant.
  const std::filesystem::path workspace = CanonicalAccessPath(CanonicalCwd());
  std::string memory_source = EnvStr("UAGENT_INTERNAL_MEMORY_SOURCE");
  const bool memory_child = !memory_source.empty();
  bool trusted = false;
  json trusted_snapshot = nullptr;
  std::string error;
  int exit_code = 1;
  if (options.show_system_prompt) {
    trusted = ProjectConfigTrusted(&trusted_snapshot);
  }
  if (!options.show_system_prompt && !memory_child &&
      !ResolveProjectTrust(options, trusted, trusted_snapshot, error,
                           exit_code)) {
    return Failure(std::move(error), exit_code);
  }

  ConfigManager config_manager =
      ConfigManager::Capture(trusted, options.overrides);
  RuntimeConfig config = config_manager.Initialize();
  if (memory_child && !BuildMemoryExtractionPrompt(memory_source, workspace,
                                                   options.prompt, error)) {
    return Failure(std::move(error), 2);
  }
  MaintainArtifacts();
  // Keep the explicit CLI flag distinct from the configured default so a
  // resumed conversation can restore its own override.
  ApprovalMode configured_mode = ApprovalMode::kAsk;
  if (!ParseApprovalMode(config.approval, configured_mode)) {
    configured_mode = ApprovalMode::kAsk;
  }
  SetApprovalMode(ResolveApprovalMode(
      options.yolo ? PermissionOverride::kYolo : PermissionOverride::kDefault,
      configured_mode));
  if (!options.debug) {
    options.debug_path = EnvStr("UAGENT_DEBUG_LOG");
    options.debug = !options.debug_path.empty();
  }

  if (!config.web_search_effort.empty() &&
      !ValidEffort(config.web_search_effort)) {
    PrintWarning("ignoring invalid UAGENT_WEB_SEARCH_EFFORT=" +
                 config.web_search_effort);
    config.web_search_effort.clear();
  }
  auto context =
      std::make_unique<AppContext>(std::move(config), std::move(config_manager),
                                   std::move(options), observability, channel);
  channel_input.Transfer();
  if ((!context->options.prompt.empty() || channel) &&
      !context->output.Silence()) {
    return Failure("cannot redirect headless output");
  }
  if (context->options.debug &&
      !observability.StartDebug(context->options.debug_path)) {
    return Failure("cannot open debug log: " + Debug().Error());
  }
  if (Debug().Enabled()) {
    FILE* notice =
        context->options.prompt.empty() && !channel ? stdout : stderr;
    fprintf(notice, "%s· debug trace: %s%s\n", DIM(), Debug().Path().c_str(),
            RST());
    Debug().Write("process_start",
                  {{"pid", getpid()},
                   {"cwd", std::filesystem::current_path().string()},
                   {"executable", ExecutablePath()},
                   {"tty", g_tty}});
  }
  if (!context->curl.Ready()) {
    return Failure("cannot initialize libcurl");
  }

  Api& api = context->runtime.api;
  size_t project_limit =
      static_cast<size_t>(context->runtime.config.project_doc_bytes);
  ProjectInstructions instructions = LoadInstructions(
      workspace, context->runtime.config, memory_child, project_limit);
  if (instructions.truncated) {
    PrintWarning("project instructions truncated at " +
                 std::to_string(project_limit) + " bytes");
  }
  if (instructions.memory_truncated) {
    PrintWarning("memory context truncated at " +
                 std::to_string(instructions.memory_limit) + " bytes");
  }
  std::vector<Skill> skills =
      memory_child ? std::vector<Skill>{} : LoadSkills(CanonicalCwd());

  context->provider = ConfigureProvider(api);
  PrintWarning(context->provider.warning);
  if (api.base_url.empty() && !context->options.show_system_prompt) {
    DebugLog("startup_error", {{"error", "UAGENT_BASE_URL is not set"}});
    return Failure(
        "no provider configured — set OPENROUTER_API_KEY or point "
        "UAGENT_BASE_URL at a supported API endpoint, e.g.\n"
        "  export UAGENT_BASE_URL=http://localhost:8080/v1");
  }
  if (!context->options.show_system_prompt && !ProbeModel(api)) {
    DebugLog("startup_error",
             {{"error", "no usable model"}, {"base_url", api.base_url}});
    return Failure("UAGENT_MODEL is not set and " + api.base_url +
                   "/models returned nothing usable");
  }
  ActivateRoute(api);
  context->tool_policy = ToolPolicyFromEnvironment();
  PrintWarning(context->tool_policy.error);
  std::string tool_error;
  context->tools =
      BuildTools(*context, workspace, trusted_snapshot, skills, tool_error);
  if (!tool_error.empty()) return Failure(tool_error);
  for (auto& tool : context->tools) {
    if (tool.name == "adapt_system") {
      tool = AdaptSystemTool(context->runtime.adaptive_system,
                             [app = context.get()](const json& request) {
                               return app->agent->PromptConfiguration(request);
                             });
    }
  }
  context->permission_override.store(context->options.yolo
                                         ? PermissionOverride::kYolo
                                         : PermissionOverride::kDefault);
  AppContext* app = context.get();
  context->agent = std::make_unique<Agent>(
      api, context->tools, context->runtime.processes,
      context->runtime.side_usage, MakeApprover(app), MakeToolRefresher(app),
      std::move(instructions), std::move(skills),
      &context->runtime.adaptive_system);
  LogReady(*context);
  return {std::move(context), {}, 0};
}

}  // namespace uagent
