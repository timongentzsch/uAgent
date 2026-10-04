// Copyright 2026 Timon Gentzsch

#include "include/app/bootstrap.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
#include "include/agent/session_store.h"
#include "include/api.h"
#include "include/app/artifact.h"
#include "include/app/asset_store.h"
#include "include/app/commands.h"
#include "include/app/config_proposal.h"
#include "include/app/coordinator.h"
#include "include/app/permissions.h"
#include "include/app/reference.h"
#include "include/app/session.h"
#include "include/app/uagent_tool.h"
#include "include/browser/browser.h"
#include "include/cli.h"
#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/project.h"
#include "include/core/runtime_config.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/skills.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/mcp/discover.h"
#include "include/mcp/register.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/tools/adapt_system.h"
#include "include/tools/ask.h"
#include "include/tools/browser.h"
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
  if (mcp_present && !trusted) {
    if (!InteractiveApprovalAvailable() || !options.prompt.empty()) {
      error =
          "project .mcp.json is untrusted; rerun with "
          "--trust-project-config after reviewing it";
      exit_code = 2;
      return false;
    }
    trusted = Confirm(
        {.kind = "project.trust",
         .prompt = "Trust this workspace's .mcp.json?",
         .options = json::array({{{"value", "y"}, {"label", "Trust"}},
                                 {{"value", "n"}, {"label", "Decline"}}})});
    if (trusted && !TrustProjectConfig(error, &trusted_snapshot)) {
      error = "cannot save project trust: " + error;
      return false;
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

// Snapshots a file into the session's committed assets, so every client can
// show it; `name` defaults to the file's own.
session::AssetStoreResult StoreSessionFile(const std::string& session_path,
                                           const std::string& path,
                                           const std::string& name,
                                           bool tool_copy) {
  session::AssetStoreResult stored;
  std::string bytes;
  if (!ReadRegularFile(path, session::kUploadBytes, bytes, stored.error)) {
    return stored;
  }
  return session::SessionAssets().Store(
      session_path, bytes,
      name.empty() ? std::filesystem::path(path).filename().string() : name,
      /*committed=*/true, tool_copy);
}

// Project docs and memory context share one byte budget: what the project
// instructions do not spend is what the memory index may.
ProjectInstructions LoadInstructions(const std::filesystem::path& workspace,
                                     const RuntimeConfig& config,
                                     bool memory_child, bool coordinator) {
  ProjectInstructions instructions;
  if (!memory_child) {
    instructions =
        LoadProjectInstructions(workspace, kProjectDocBytes, coordinator);
  }
  if (config.memory_enabled) {
    size_t remaining = instructions.text.size() >= kProjectDocBytes
                           ? 0
                           : kProjectDocBytes - instructions.text.size();
    MemoryIndex memories = LoadMemoryIndex(workspace, remaining);
    instructions.memory_index = std::move(memories.text);
    instructions.memory_sources = std::move(memories.sources);
    instructions.memory_truncated |= memories.truncated;
    instructions.memory_limit = remaining;

    // A delegated child's brief is standalone: re-inlining every always-on
    // memory there only duplicates the parent's context.
    if (AgentDepth() == 0) {
      MemoryIndex always = LoadAlwaysOnMemory(workspace, kMemoryAlwaysBytes);
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
        instructions.memory_limit = kMemoryAlwaysBytes;
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
  const std::string toolset = EnvStr("UAGENT_INTERNAL_TOOLSET");
  const std::string session_path =
      context.channel ? context.channel->SessionPath() : std::string();
  std::vector<Tool> tools = BuiltinTools(runtime.processes, workspace);
  if (AdaptiveSystemEnabled()) {
    // The agent exists by the time a tool runs.
    tools.push_back(AdaptSystemTool(runtime.adaptive_system,
                                    [app = &context](const json& request) {
                                      return app->agent->SelfDirective(request);
                                    }));
  }
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
      return PrepareConfigProposal(scope, changes, app->config_manager);
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
  if (!session_path.empty() && AgentDepth() == 0) {
    tools.push_back(ArtifactTool(session_path));
  }
  // Only a session someone can answer gets ask: never headless runs or
  // delegated children, which could only ever time out.
  if (context.channel && InteractiveApprovalAvailable()) {
    // Option images are snapshotted into the session, as artifacts are, so
    // every client can show them.
    AskImage image;
    if (!session_path.empty()) {
      image = [session_path](const std::string& path, std::string& failure) {
        session::AssetStoreResult stored =
            StoreSessionFile(session_path, path, "", /*tool_copy=*/false);
        failure = stored.error;
        return stored.value;
      };
    }
    tools.push_back(AskTool(
        [](const json& questions, bool* eof) {
          return ReadInteraction(
              {.kind = "ask",
               .prompt = JsonValue(questions[0], "question", ""),
               .questions = questions},
              eof);
        },
        std::move(image)));
  }
#ifdef UAGENT_BROWSER  // the web host starts the browser and serves its viewer
  if (!browser::DataDirectory().empty() && context.options.browser_session &&
      !session_path.empty() && AgentDepth() == 0) {
    tools.push_back(BrowserTool(
        HashHex(session_path),
        [](const std::string& id, const std::string& prompt, bool* eof) {
          return ReadInteraction(
              {.id = id, .kind = "browser", .prompt = prompt}, eof);
        }));
  }
#endif
  // The default lean child is an isolation and context-efficiency boundary:
  // do not clone the parent's entire MCP fleet into every delegation. A root
  // lean session and an explicitly requested full child still get MCP.
  if (AgentDepth() == 0 || toolset != "lean") {
    error = McpRegister(tools, runtime.mcp, runtime.config, trusted_snapshot);
    if (!error.empty()) return {};
  }
  if (CanDelegate()) {
    tools.push_back(
        SubagentTool(api, runtime.processes, context.provider.routes,
                     context.provider.providers, context.options.debug));
  }
  // Peer sessions are text-only and isolation-gated by links, so the session
  // tool is safe in every toolset, lean included.
  tools.push_back(SessionTool([](const std::string& path) {
    std::string ignored;  // the mail is queued either way
    session::Open(ExecutablePath(),
                  JsonValue(SessionHeader(path), kSessionHeaderCwd, ""), path,
                  "", Options{}, ignored);
  }));
  if (context.options.Coordinator()) {
    AddCoordinatorTools(tools, CanonicalCwd());
  }
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

// What a person may answer an ordinary approval with: the option each client
// offers and every spelling the terminal accepts for it. An answer outside
// the table is guidance: denied, and queued as steering.
enum class ApprovalAnswer { kOnce, kSession, kRepository, kDeny, kGuidance };

struct ApprovalChoice {
  ApprovalAnswer answer;
  std::string_view value;
  std::string_view label;
  std::array<std::string_view, 3> aliases;
};

constexpr ApprovalChoice kApprovalChoices[] = {
    {ApprovalAnswer::kOnce, "y", "Allow once", {"y", "yes"}},
    {ApprovalAnswer::kSession, "s", "Allow for this session", {"s", "session"}},
    {ApprovalAnswer::kRepository,
     "a",
     "Always in this repository",
     {"a", "always", "repository"}},
    {ApprovalAnswer::kDeny, "n", "Deny", {"n", "no"}},
    {ApprovalAnswer::kGuidance, "guidance", "Send guidance", {}},
};

json ApprovalOptions() {
  json options = json::array();
  for (const ApprovalChoice& choice : kApprovalChoices) {
    options.push_back({{"value", choice.value}, {"label", choice.label}});
  }
  return options;
}

ApprovalAnswer ParseApprovalAnswer(const std::string& answer) {
  const std::string lowered = AsciiLower(answer);
  if (lowered.empty()) return ApprovalAnswer::kGuidance;
  for (const ApprovalChoice& choice : kApprovalChoices) {
    if (std::ranges::find(choice.aliases, lowered) != choice.aliases.end()) {
      return choice.answer;
    }
  }
  return ApprovalAnswer::kGuidance;
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
    // Who refused, for the model: a person is the default.
    std::string refusal = "user denied this action";
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
      const bool ask = decision.decision == AutoPermissionDecision::kAsk;
      const bool allow = decision.decision == AutoPermissionDecision::kAllow;
      review = {{"choice", ask ? "ask" : (allow ? "allow" : "deny")},
                {"probabilities", std::move(decision.probabilities)}};
      if (!decision.error.empty()) review["error"] = decision.error;
      // An undecided review with nobody to ask is a denial.
      if (!ask || !InteractiveApprovalAvailable()) {
        automatic = true;
        granted = allow;
        refusal = ask ? "the automatic permission review could not decide "
                        "this action and nobody is here to ask"
                      : "the automatic permission review refused this action";
        if (!decision.error.empty()) refusal += " (" + decision.error + ")";
      }
      DebugLog("permission_review", {{"tool", tool.name}, {"result", review}});
    }
    if (!automatic) {
      // Reaching µAgent's own configuration is the reason a tool escalates by
      // its path; a tool that escalates for its own reason names it.
      std::string reason = tool.mandatory_reason.empty()
                               ? "changes \u00b5Agent's own configuration"
                               : tool.mandatory_reason;
      // The request carries the full command/payload, so long commands are
      // never truncated. A tool with more to show than its one-line label
      // supplies its own preview.
      Emit(Event{EventId::kApprovalRequested,
                 {{"id", request_id},
                  {"tool", tool.name},
                  {"preview", TerminalSafe(raw_payload)},
                  {"risks", ApprovalRisks(tool, arguments, root)},
                  {"mandatory_human", mandatory},
                  {"mandatory_reason", mandatory ? reason : std::string()}}});
      InteractionRequest request{
          .id = request_id,
          .kind = "approval",
          .prompt = "Allow " + TerminalSafe(tool.name) + "?"};
      if (mandatory && !InteractiveApprovalAvailable()) {
        granted = false;
        refusal = "this action needs a person's approval (" + reason +
                  ") and nobody is here to give it";
      } else if (mandatory) {
        request.options = json::array({{{"value", "y"}, {"label", "Allow"}},
                                       {{"value", "n"}, {"label", "Deny"}}});
        granted = Confirm(std::move(request));
      } else {
        request.options = ApprovalOptions();
        bool cancelled = false;
        bool eof = false;
        std::string answer =
            Trim(ReadChoiceLine(std::move(request), cancelled, eof));
        const ApprovalAnswer chosen = cancelled || eof
                                          ? ApprovalAnswer::kDeny
                                          : ParseApprovalAnswer(answer);
        granted = chosen == ApprovalAnswer::kOnce ||
                  chosen == ApprovalAnswer::kSession ||
                  chosen == ApprovalAnswer::kRepository;
        if (chosen == ApprovalAnswer::kSession) {
          app->session_approvals.insert(key);
        }
        if (chosen == ApprovalAnswer::kRepository) {
          std::string error;
          if (!RememberRepositoryPermission(root, key, tool.name, raw_payload,
                                            error)) {
            Emit(NoticeEvent(
                PresentationStatus::kWarned,
                "allowed once; could not save permission rule: " + error));
          }
        }
        if (chosen == ApprovalAnswer::kGuidance && !answer.empty()) {
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
    return granted ? std::string() : refusal;
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
  const SandboxStatus& status = SandboxRuntime();
  if (status.mode == SandboxMode::kDegraded) {
    Emit(Event{EventId::kCapabilityChanged,
               {{"feature", "sandbox"},
                {"from", true},
                {"to", false},
                {"reason", status.reason}}});
    Emit(
        NoticeEvent(PresentationStatus::kWarned,
                    "sandbox: " + status.reason + "; commands run unconfined"));
  }
  if (status.rejected.empty()) return;
  std::string dropped;
  for (const std::string& root : status.rejected) {
    dropped += (dropped.empty() ? "" : ", ") + root;
  }
  Emit(NoticeEvent(PresentationStatus::kWarned,
                   "sandbox: not granted as writable: " + dropped));
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
  // The behavior switches are recorded in the provenance and repeated at the
  // top level of the event.
  const json behavior = {
      {"reasoning_effort", api.reasoning_effort},
      {"openrouter_variant", config.openrouter_variant},
      {"context_window", api.ctx_window},
      {"memory", config.memory_enabled},
      {"memory_generate", config.memory_generate},
      {"run_mode", run_mode},
      {"approval", ApprovalModeName(CurrentApprovalMode())},
      {"auto_compact_pct", AutoCompactPct()},
      {"auto_compact_tokens", AutoCompactTokens()},
      {"tool_result_chars", ToolResultCap()},
      {"tool_batch_result_chars", ToolBatchResultCap()},
      {"adaptive_system", AdaptiveSystemEnabled()},
      {"max_tokens", MaxOutputTokens()},
      {"prompt_overlay",
       overlay_digest.empty() ? json(nullptr) : json(overlay_digest)},
  };
  provenance["behavior"] = behavior;
  json ready = {
      {"base_url", RedactedUrl(api.base_url)},
      {"model", api.RequestModel()},
      {"route", RouteSelection(api, context.provider.providers)},
      {"provenance", std::move(provenance)},
      {"capabilities", api.capabilities.DiagnosticJson()},
      {"configured_models", context.provider.routes.size()},
      {"tools", context.tools.size()},
      {"toolset", toolset},
      {"output_mode", context.options.json_stream
                          ? "json-stream"
                          : (context.options.json ? "json" : "text")},
      {"yolo", ApprovalIsYolo()},
      {"openrouter_provider", config.openrouter_provider},
      {"attachment_mb", AttachmentLimitMb()},
      {"limits", config.DiagnosticJson()},
      {"effective_config", context.config_manager.DiagnosticJson(config)}};
  ready.update(behavior);
  Emit(Event{EventId::kSessionReady, std::move(ready)});
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

  // Only the flag vouches for a config file an earlier version left in the
  // project; otherwise it is taken over only as it was approved.
  ConfigManager config_manager = ConfigManager::Capture(
      options.trust_project || TrustProjectConfig(), options.overrides);
  RuntimeConfig config = config_manager.Initialize();
  PrintWarning(config_manager.Problem());
  // Route resolution reads UAGENT_MODEL; a coordinator starts on its own
  // model. A /model saved in its session still wins on resume.
  if (options.Coordinator() && !options.overrides.contains("UAGENT_MODEL")) {
    const std::string model = CoordinatorModel();
    if (!model.empty()) OverrideSetting("UAGENT_MODEL", model);
  }
  if (memory_child && !BuildMemoryExtractionPrompt(memory_source, workspace,
                                                   options.prompt, error)) {
    return Failure(std::move(error), 2);
  }
  MaintainArtifacts();
  if (!options.debug) {
    options.debug_path = SettingText(Cfg("UAGENT_DEBUG_LOG"));
    options.debug = !options.debug_path.empty();
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
    // Every bootstrapped run has stdout silenced by now.
    fputs(Note(Tone::kNeutral, "debug trace: " + Debug().Path()).c_str(),
          stderr);
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
  ProjectInstructions instructions =
      LoadInstructions(workspace, context->runtime.config, memory_child,
                       context->options.Coordinator());
  if (instructions.truncated) {
    PrintWarning("project instructions truncated at " +
                 std::to_string(kProjectDocBytes) + " bytes");
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
  // A flag for a conversation's setting sets it for that conversation, and
  // is kept with it: a run started with a model and a mode (a scheduled
  // task's) still has them when its runtime starts again without the flag.
  for (const ConfigDescriptor& setting : ConfigRegistry()) {
    const auto flag =
        context->options.overrides.find(std::string(setting.environment));
    if ((setting.scopes & kScopeConversation) &&
        flag != context->options.overrides.end()) {
      context->config_manager.ChooseForConversation(flag->first, flag->second);
    }
  }
  if (context->options.yolo) {
    context->config_manager.ChooseForConversation("UAGENT_APPROVAL", "yolo");
  }
  // The mode in effect, resolved where it always is: scopes, then the role.
  // Before the tools, whose servers are started with it.
  PermissionControl(*context, json::object());
  context->tool_policy = ToolPolicyFromEnvironment();
  if (context->options.Coordinator()) {
    context->tool_policy.tool_allowlist.assign(std::begin(kCoordinatorTools),
                                               std::end(kCoordinatorTools));
  }
  PrintWarning(context->tool_policy.error);
  std::string tool_error;
  context->tools =
      BuildTools(*context, workspace, trusted_snapshot, skills, tool_error);
  if (!tool_error.empty()) return Failure(tool_error);
  AppContext* app = context.get();
  context->agent = std::make_unique<Agent>(
      api, context->tools, context->runtime.processes,
      context->runtime.side_usage, MakeApprover(app), MakeToolRefresher(app),
      std::move(instructions), std::move(skills),
      &context->runtime.adaptive_system);
  context->agent->SetSessionRole(context->options.session);
  if (context->options.Coordinator()) {
    context->agent->SetRuntimeContext(
        [folder = CanonicalCwd(), agent = context->agent.get()] {
          RecordCoordinatorCost(folder, agent->SessionUsage().cost);
          return CoordinatorContext(folder);
        });
  }
  if (context->channel && !context->channel->SessionPath().empty()) {
    context->agent->OpenEditJournal(context->channel->SessionPath() + ".edits");
    context->agent->KeepToolFiles(
        [session_path = context->channel->SessionPath()](
            const std::string& path, const std::string& name) -> json {
          session::AssetStoreResult stored =
              StoreSessionFile(session_path, path, name, /*tool_copy=*/true);
          return stored.error.empty() ? std::move(stored.value) : json(nullptr);
        });
  }
  LogReady(*context);
  return {std::move(context), {}, 0};
}

}  // namespace uagent
