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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/cli.h"
#include "include/core/config.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp/register.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/memory.h"
#include "include/tools/registry.h"
#include "include/tools/skill.h"
#include "include/tools/subagent.h"
#include "include/tools/web_search.h"
#include "include/ui/display.h"

namespace uagent {
namespace {

BootstrapResult Failure(std::string error, int exit_code = 1) {
  return {nullptr, std::move(error), exit_code};
}

void PrintWarning(const std::string& warning) {
  if (!warning.empty()) {
    fprintf(stderr, "%s%s%s\n", YEL(), TerminalSafe(warning).c_str(), RST());
  }
}

// A y/N (or Y/n) prompt. EOF always declines: an unattended run must never
// grant trust or approval by accident.
bool Confirm(const std::string& question, bool default_yes) {
  bool eof = false;
  std::string answer =
      Trim(ReadInputLine(question, &eof, /*keep_history=*/false));
  if (eof) return false;
  if (answer.empty()) return default_yes;
  return answer == "y" || answer == "Y" || answer == "yes";
}

bool ResolveProjectTrust(const Options& options, bool& trusted,
                         json& trusted_snapshot, std::string& error,
                         int& exit_code) {
  trusted =
      options.trust_project || EnvStr("UAGENT_TRUST_PROJECT_CONFIG") == "1";
  if (!trusted) trusted = ProjectConfigTrusted(&trusted_snapshot);
  bool mcp_present = ProjectMcpPresent();
  bool agent_config_present = ProjectAgentConfigPresent();
  if ((mcp_present || agent_config_present) && !trusted) {
    std::string surfaces =
        mcp_present ? (agent_config_present ? ".mcp.json and .uagent/.config"
                                            : ".mcp.json")
                    : ".uagent/.config";
    if (!isatty(STDIN_FILENO) || !options.prompt.empty()) {
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
      trusted = Confirm("Trust this workspace's " + surfaces + "? [y/N] ",
                        /*default_yes=*/false);
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

void PrintStartupRow(std::string_view label, std::string summary,
                     std::vector<std::string> details) {
  constexpr size_t kLabelWidth = 7;
  std::string padded_label(label);
  padded_label.append(kLabelWidth - padded_label.size(), ' ');
  std::string first_prefix = "  " + padded_label + " │ ";
  std::string next_prefix = "  " + std::string(kLabelWidth, ' ') + " │ ";
  size_t prefix_width = DisplayWidth(first_prefix);
  size_t columns = static_cast<size_t>(std::max(int64_t{1}, TerminalColumns()));
  size_t value_width = columns > prefix_width ? columns - prefix_width : 1;
  details.insert(details.begin(), std::move(summary));
  bool first = true;
  for (const std::string& detail : details) {
    for (const std::string& line :
         WrapLines(TerminalSafe(detail), value_width)) {
      printf("%s%s%s%s\n", DIM(),
             first ? first_prefix.c_str() : next_prefix.c_str(), line.c_str(),
             RST());
      first = false;
    }
  }
}

void PrintProjectContext(const ProjectInstructions& instructions,
                         size_t byte_limit) {
  if (!instructions.sources.empty() || !instructions.memory_sources.empty()) {
    std::string cwd = CanonicalCwd() + "/";
    std::vector<std::string> sources = instructions.sources;
    sources.insert(sources.end(), instructions.memory_sources.begin(),
                   instructions.memory_sources.end());
    std::vector<std::string> display;
    display.reserve(sources.size());
    for (const std::string& source : sources) {
      display.push_back(source.starts_with(cwd) ? source.substr(cwd.size())
                                                : Tilde(source));
    }
    PrintStartupRow("Context", std::to_string(sources.size()) + " sources",
                    std::move(display));
  }
  if (instructions.truncated) {
    PrintWarning("project instructions truncated at " +
                 std::to_string(byte_limit) + " bytes");
  }
}

void PrintSkills(const std::vector<Skill>& skills) {
  if (skills.empty()) return;
  std::string list;
  for (const Skill& skill : skills) {
    if (!list.empty()) list += ", ";
    list += skill.name;
  }
  PrintStartupRow("Skills", std::to_string(skills.size()) + " available",
                  {std::move(list)});
}

bool ProbeModel(Api& api) {
  if (!api.model.empty() && (api.ctx_window > 0 || api.openrouter_compatible)) {
    return true;
  }
  auto started = std::chrono::steady_clock::now();
  json models = api.Get("/models");
  size_t offered = 0;
  if (models.is_object() && models.contains("data") &&
      models["data"].is_array()) {
    const json& data = models["data"];
    offered = data.size();
    if (api.model.empty()) {
      for (const json& candidate : data) {
        if (!candidate.is_object()) continue;
        api.model = JsonValue(candidate, "id", "");
        if (!api.model.empty()) break;
      }
    }
    std::string base = api.model.substr(0, api.model.find(':'));
    if (api.ctx_window == 0) {
      for (const json& model : data) {
        if (!model.is_object()) continue;
        std::string id = JsonValue(model, "id", "");
        if (id != api.model && id != base) continue;
        api.ctx_window = CatalogContextLength(model);
        break;
      }
    }
  }
  DebugLog("models_probe", {{"duration_ms", ElapsedMs(started)},
                            {"models_offered", offered},
                            {"model", api.model},
                            {"context_window", api.ctx_window}});
  return !api.model.empty();
}

std::vector<Tool> BuildTools(AppContext& context,
                             const std::filesystem::path& workspace,
                             const json& trusted_snapshot,
                             std::vector<Skill> skills) {
  Api& api = context.runtime.api;
  AppRuntime& runtime = context.runtime;
  bool inline_images =
      context.options.prompt.empty() && g_tty &&
      DetectTerminalImageProtocol() != TerminalImageProtocol::kNone;
  std::vector<Tool> tools = BuiltinTools(
      runtime.processes, workspace, inline_images,
      AdaptiveSystemEnabled() ? &runtime.adaptive_system : nullptr);
  if (!runtime.config.memory_enabled) {
    std::erase_if(tools,
                  [](const Tool& tool) { return tool.name == "memory"; });
  }
  if (EnvStr("UAGENT_TOOLSET") == "memory") {
    std::erase_if(tools,
                  [](const Tool& tool) { return tool.name != "memory"; });
    return tools;
  }
  WebSearchRoute search_route =
      SelectWebSearchRoute(api, context.provider.providers);
  if (search_route.Valid()) {
    tools.push_back(
        WebSearchTool(api, runtime.side_usage, context.provider.providers));
  }
  McpRegister(tools, runtime.mcp, runtime.config, trusted_snapshot);
  if (CanDelegate()) {
    tools.push_back(
        SubagentTool(api, runtime.processes, context.provider.routes,
                     context.provider.providers, context.options.debug));
  }
  if (LeanToolset()) KeepLeanTools(tools);
  ApplyToolPolicy(tools, context.tool_policy);
  KeepSupportedSkills(skills, tools);
  if (!skills.empty()) {
    std::vector<Tool> skill_tool{SkillTool(skills)};
    ApplyToolPolicy(skill_tool, context.tool_policy);
    if (!skill_tool.empty()) {
      PrintSkills(skills);
      tools.push_back(std::move(skill_tool.front()));
    }
  }
  return tools;
}

void LogReady(const AppContext& context) {
  if (!Debug().Enabled()) return;
  const Api& api = context.runtime.api;
  const RuntimeConfig& config = context.runtime.config;
  Debug().Write(
      "session_ready",
      {{"base_url", api.base_url},
       {"model", api.RequestModel()},
       {"reasoning_effort", api.reasoning_effort},
       {"openrouter_variant", config.openrouter_variant},
       {"openrouter_compatible", api.openrouter_compatible},
       {"configured_models", context.provider.routes.size()},
       {"context_window", api.ctx_window},
       {"tools", context.tools.size()},
       {"toolset", LeanToolset() ? "lean" : "full"},
       {"memory", config.memory_enabled},
       {"memory_generate", config.memory_generate},
       {"run_mode",
        context.options.prompt.empty() ? "interactive" : "headless"},
       {"output_mode", context.options.json_stream
                           ? "json-stream"
                           : (context.options.json ? "json" : "text")},
       {"yolo", context.options.yolo},
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
       {"limits", config.DiagnosticJson()}});
}

}  // namespace

HeadlessOutput::~HeadlessOutput() { Restore(); }

bool HeadlessOutput::Silence() {
  if (saved_stdout_ >= 0) return true;
  fflush(stdout);
  saved_stdout_ = dup(STDOUT_FILENO);
  if (saved_stdout_ < 0) return false;
  fcntl(saved_stdout_, F_SETFD, FD_CLOEXEC);
  int null_fd = open("/dev/null", O_WRONLY);
  if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) {
    if (null_fd >= 0) close(null_fd);
    Restore();
    return false;
  }
  close(null_fd);
  return true;
}

void HeadlessOutput::Restore() {
  if (saved_stdout_ < 0) return;
  fflush(stdout);
  dup2(saved_stdout_, STDOUT_FILENO);
  close(saved_stdout_);
  saved_stdout_ = -1;
}

AppContext::AppContext(RuntimeConfig config, Options parsed_options)
    : runtime(std::move(config)), options(std::move(parsed_options)) {}

BootstrapResult Bootstrap(Options options, const char* executable) {
  SetExecutablePath(executable);
  // Nothing chdir()s during startup, so the canonical workspace is invariant.
  const std::filesystem::path workspace = CanonicalAccessPath(CanonicalCwd());
  std::string memory_source = EnvStr("UAGENT_INTERNAL_MEMORY_SOURCE");
  const bool memory_child = !memory_source.empty();
  bool trusted = false;
  json trusted_snapshot = nullptr;
  std::string error;
  int exit_code = 1;
  if (!memory_child && !ResolveProjectTrust(options, trusted, trusted_snapshot,
                                            error, exit_code)) {
    return Failure(std::move(error), exit_code);
  }

  LoadConfigFile(trusted);
  if (memory_child && !BuildMemoryExtractionPrompt(memory_source, workspace,
                                                   options.prompt, error)) {
    return Failure(std::move(error), 2);
  }
  if (options.budget > 0) {
    setenv("UAGENT_SESSION_BUDGET", std::to_string(options.budget).c_str(), 1);
  }
  if (options.no_memory) {
    setenv("UAGENT_MEMORY", "0", 1);
  }
  MaintainArtifacts();
  if (!options.yolo) options.yolo = EnvStr("UAGENT_APPROVAL") == "yolo";
  if (!options.debug) {
    options.debug_path = EnvStr("UAGENT_DEBUG_LOG");
    options.debug = !options.debug_path.empty();
  }

  RuntimeConfig config = RuntimeConfig::FromEnvironment();
  if (!config.web_search_effort.empty() &&
      !ValidEffort(config.web_search_effort)) {
    PrintWarning("ignoring invalid UAGENT_WEB_SEARCH_EFFORT=" +
                 config.web_search_effort);
    config.web_search_effort.clear();
  }
  auto context =
      std::make_unique<AppContext>(std::move(config), std::move(options));
  if (!context->options.prompt.empty() && !context->output.Silence()) {
    return Failure("cannot redirect headless output");
  }
  if (context->options.debug && !Debug().Start(context->options.debug_path)) {
    return Failure("cannot open debug log: " + Debug().Error());
  }
  if (Debug().Enabled()) {
    FILE* notice = context->options.prompt.empty() ? stdout : stderr;
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
  api.render_stream = context->options.prompt.empty();
  size_t project_limit =
      static_cast<size_t>(context->runtime.config.project_doc_bytes);
  ProjectInstructions instructions;
  if (!memory_child) {
    instructions = LoadProjectInstructions(workspace, project_limit);
  }
  if (context->runtime.config.memory_enabled) {
    size_t remaining = instructions.text.size() >= project_limit
                           ? 0
                           : project_limit - instructions.text.size();
    MemoryIndex memories = LoadMemoryIndex(workspace, remaining);
    instructions.memory_index = std::move(memories.text);
    instructions.memory_sources = std::move(memories.sources);
    instructions.truncated |= memories.truncated;

    size_t always_bytes =
        static_cast<size_t>(std::max(int64_t{0}, MemoryAlwaysBytes()));
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
      instructions.truncated |= always.truncated;
    }
  }
  printf("%s%sµAgent%s\n", RST(), BOLD(), RST());
  PrintProjectContext(instructions, project_limit);
  std::vector<Skill> skills =
      memory_child ? std::vector<Skill>{} : LoadSkills(CanonicalCwd());

  context->provider = ConfigureProvider(api);
  PrintWarning(context->provider.warning);
  if (api.base_url.empty()) {
    DebugLog("startup_error", {{"error", "UAGENT_BASE_URL is not set"}});
    return Failure(
        "no provider configured — set OPENROUTER_API_KEY or point "
        "UAGENT_BASE_URL at an OpenAI-compatible endpoint, e.g.\n"
        "  export UAGENT_BASE_URL=http://localhost:8080/v1");
  }
  if (!ProbeModel(api)) {
    DebugLog("startup_error",
             {{"error", "no usable model"}, {"base_url", api.base_url}});
    return Failure("UAGENT_MODEL is not set and " + api.base_url +
                   "/models returned nothing usable");
  }
  ActivateRoute(api);
  context->tool_policy = ToolPolicyFromEnvironment();
  PrintWarning(context->tool_policy.error);
  context->tools = BuildTools(*context, workspace, trusted_snapshot, skills);
  AppContext* app = context.get();
  context->agent = std::make_unique<Agent>(
      api, context->tools, context->runtime.processes,
      context->runtime.side_usage,
      [app](const Tool& tool, const json& arguments) {
        bool granted = true;
        if (!app->options.yolo) {
          // Print the full command/payload before asking, so long commands are
          // never truncated in the approval prompt.
          std::string payload = TerminalSafe(ToolSummary(tool, arguments));
          fprintf(stdout, "%sallow %s%s\n%s\n%s\n", YEL(),
                  TerminalSafe(tool.name).c_str(), RST(), payload.c_str(),
                  RST());
          std::string question = std::string(YEL()) + "allow " +
                                 TerminalSafe(tool.name) + "? [Y/n] " + RST();
          granted = Confirm(question, /*default_yes=*/true);
        }
        DebugLog("approval", {{"tool", tool.name},
                              {"automatic", app->options.yolo},
                              {"granted", granted}});
        return granted;
      },
      [app](std::chrono::steady_clock::time_point deadline) {
        bool changed = McpRefreshTools(app->tools, app->runtime.mcp,
                                       app->runtime.config, deadline);
        if (changed) ApplyToolPolicy(app->tools, app->tool_policy);
        return changed;
      },
      std::move(instructions), std::move(skills),
      &context->runtime.adaptive_system);
  LogReady(*context);
  return {std::move(context), {}, 0};
}

}  // namespace uagent
