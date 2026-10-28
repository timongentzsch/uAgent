// Copyright 2026 Timon Gentzsch

#include "include/app/bootstrap.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/prompt.h"
#include "include/agent/session_store.h"
#include "include/api/client.h"
#include "include/app/asset_store.h"
#include "include/app/commands.h"
#include "include/app/panel.h"
#include "include/app/self_description.h"
#include "include/app/session.h"
#include "include/cli/cli.h"
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
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/media/attachments.h"
#include "include/providers/providers.h"
#include "include/tools/memory.h"
#include "include/tools/registry.h"
#include "include/ui/display.h"
#include "include/ui/presentation.h"
#include "src/app/bootstrap_internal.h"

namespace uagent {
namespace {

BootstrapResult Failure(std::string error, int exit_code = 1) {
  return {nullptr, std::move(error), exit_code};
}

// A run nobody can answer never picks a model for the person: it says what the
// endpoint offers.
std::string NoModelMessage(Api& api) {
  constexpr size_t kListed = 10;
  std::string message = "no model set: set `model` (UAGENT_MODEL)";
  std::optional<std::vector<ModelInfo>> models = CachedModels(api);
  if (!models) {
    models = QueryModels(api);
  }
  if (!models || models->empty()) {
    return message + "; " + UrlHost(api.base_url) + " lists no models";
  }
  message += "; " + UrlHost(api.base_url) + " offers:";
  for (size_t i = 0; i < models->size() && i < kListed; ++i) {
    message += (i ? ", " : " ") + (*models)[i].id;
  }
  if (models->size() > kListed) {
    message += ", and " + std::to_string(models->size() - kListed) + " more";
  }
  return message;
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

bool ResolveProjectTrust(const Options& options, bool& trusted,
                         json& trusted_snapshot, std::string& error,
                         int& exit_code) {
  trusted = options.trust_project;
  if (!trusted) trusted = ProjectConfigTrusted(&trusted_snapshot);
  bool mcp_present = ProjectMcpPresent();
  if (mcp_present && !trusted) {
    if (!InteractiveApprovalAvailable() || !options.prompt.empty()) {
      error =
          "this folder's .mcp.json is untrusted; rerun with "
          "--trust-project-config after reviewing it";
      exit_code = 2;
      return false;
    }
    // Said where it is approved: a server that opts out of the sandbox is the
    // part of this file that can reach what the sandbox keeps from commands.
    std::string unconfined;
    json file;
    std::string unread;
    const json* servers = ProjectMcpSnapshot(file, unread)
                              ? JsonObject(file, "mcpServers")
                              : nullptr;
    if (servers) {
      for (const auto& [name, server] : servers->items()) {
        if (JsonValue(server, "sandbox", true)) continue;
        unconfined += (unconfined.empty() ? "" : ", ") + name;
      }
    }
    trusted = Confirm(
        {.kind = "project.trust",
         .prompt = "Trust this folder's .mcp.json?" +
                   (unconfined.empty()
                        ? std::string()
                        : " It runs unconfined, outside the sandbox: " +
                              unconfined + "."),
         .options = json::array({{{"value", "y"}, {"label", "Trust"}},
                                 {{"value", "n"}, {"label", "Decline"}}})});
    if (trusted && !TrustProjectConfig(error, &trusted_snapshot)) {
      error = "cannot save folder trust: " + error;
      return false;
    }
  }
  if (mcp_present && trusted && trusted_snapshot.is_null()) {
    if (!ProjectMcpSnapshot(trusted_snapshot, error)) {
      error = "cannot load trusted folder config: " + error;
      return false;
    }
  }
  return true;
}

// Project docs and memory context share one byte budget: what the project
// instructions do not spend is what the memory index may.
ProjectInstructions LoadInstructions(const std::filesystem::path& workspace,
                                     const RuntimeConfig& config,
                                     bool memory_child, bool panel) {
  ProjectInstructions instructions;
  if (!memory_child) {
    instructions = LoadProjectInstructions(workspace, kProjectDocBytes, panel);
  }
  if (config.memory_enabled) {
    // A delegated child's brief is standalone: re-inlining every always-on
    // memory there only duplicates the parent's context. What the slice
    // leaves out stays in the index, so only a cut index loses anything.
    MemoryIndex always;
    if (AgentDepth() == 0) {
      always = LoadAlwaysOnMemory(workspace, kMemoryAlwaysBytes);
    }
    size_t remaining = instructions.text.size() >= kProjectDocBytes
                           ? 0
                           : kProjectDocBytes - instructions.text.size();
    MemoryIndex memories =
        LoadMemoryIndex(workspace, remaining, always.sources);
    instructions.memory_index = std::move(memories.text);
    instructions.memory_always = std::move(always.text);
    instructions.memory_sources = std::move(memories.sources);
    instructions.memory_sources.insert(instructions.memory_sources.end(),
                                       always.sources.begin(),
                                       always.sources.end());
    instructions.memory_truncated = memories.truncated;
    instructions.memory_limit = remaining;
  }
  return instructions;
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
  // The workspace is said in full: "not granted" alone does not tell a person
  // that every command in this conversation is about to fail to save its work.
  const std::string workspace = CanonicalCwd();
  // At the readonly level, a lead agent's, the workspace is not writable on
  // purpose.
  if (std::string refusal = SandboxWorkspaceRefusal();
      !refusal.empty() && !status.policy.read_only) {
    Emit(NoticeEvent(PresentationStatus::kWarned,
                     "sandbox: " + refusal +
                         ". Start in a folder or a named workspace "
                         "to give commands one"));
  }
#if defined(__linux__)
  if (!status.policy.withheld.empty()) {
    Emit(NoticeEvent(PresentationStatus::kNeutral,
                     "sandbox: relaxed -- " + SandboxNote()));
  }
#endif
  std::string dropped;
  for (const std::string& root : status.rejected) {
    if (root == workspace) continue;
    dropped += (dropped.empty() ? "" : ", ") + root;
  }
  if (dropped.empty()) return;
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
      {"context_window", api.ctx_window},
      {"memory", config.memory_enabled},
      {"memory_generate", config.memory_generate},
      {"run_mode", run_mode},
      {"approval", ApprovalModeName(CurrentApprovalMode())},
      {"auto_compact_pct", AutoCompactPct()},
      {"auto_compact_tokens", AutoCompactTokens()},
      {"tool_result_chars", ToolResultCap()},
      {"tool_batch_result_chars", ToolBatchResultCap()},
      {"max_tokens", MaxOutputTokens()},
      {"prompt_overlay",
       overlay_digest.empty() ? json(nullptr) : json(overlay_digest)},
  };
  provenance["behavior"] = behavior;
  json ready = {
      {"base_url", RedactedUrl(api.base_url)},
      {"model", api.model},
      {"route", RouteSelection(api, context.provider.providers)},
      {"provenance", std::move(provenance)},
      {"capabilities",
       api.capabilities.DiagnosticJson(api.wire_api, api.features)},
      {"configured_models", context.provider.routes.size()},
      {"tools", context.tools.size()},
      {"toolset", toolset},
      {"output_mode", context.options.json_stream
                          ? "json-stream"
                          : (context.options.json ? "json" : "text")},
      {"yolo", ApprovalIsYolo()},
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
  // A headless run's stdout is its answer and a runtime's belongs to its
  // clients: neither is drawn on.
  const bool headless = !channel && !options.prompt.empty();
  if (channel || headless) observability.EnableTerminal(false);
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

  ConfigManager config_manager = ConfigManager::Capture(options.overrides);
  RuntimeConfig config = config_manager.Initialize();
  PrintWarning(config_manager.Problem());
  if (headless && !config_manager.Problem().empty()) {
    fprintf(stderr, "uagent: %s\n", config_manager.Problem().c_str());
  }
  if (memory_child && !BuildMemoryExtractionPrompt(memory_source, workspace,
                                                   options.prompt, error)) {
    return Failure(std::move(error), 2);
  }
  // Housekeeping over the whole artifact tree is a session's, not something
  // each child it starts repeats.
  if (AgentDepth() == 0) MaintainArtifactsDaily();
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
                       context->options.Panel());
  if (instructions.truncated) {
    PrintWarning("folder instructions truncated at " +
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
  if (!context->options.show_system_prompt && api.model.empty() &&
      !context->channel) {
    DebugLog("startup_error",
             {{"error", "no model set"}, {"base_url", api.base_url}});
    return Failure(NoModelMessage(api));
  }
  // A conversation without a model starts at the model picker.
  ProbeModel(api);
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
  if (context->options.Panel()) {
    context->tool_policy.tool_allowlist.assign(std::begin(kPanelTools),
                                               std::end(kPanelTools));
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
      std::move(instructions), std::move(skills));
  context->agent->SetSessionRole(context->options.session);
  if (memory_child) context->agent->SetUsagePurpose("memory_extract");
  if (context->options.Panel()) {
    context->agent->SetRuntimeContext(
        [folder = CanonicalCwd(), agent = context->agent.get()] {
          RecordPanelCost(folder, agent->SessionUsage().cost);
          return PanelContext(folder);
        });
  }
  if (context->channel && !context->channel->SessionPath().empty()) {
    context->agent->OpenEditJournal(context->channel->SessionPath() +
                                    kSessionEditsSuffix);
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
