// Copyright 2026 Timon Gentzsch

// µAgent — a lean C++ terminal coding agent for OpenAI-compatible endpoints.
//
// Config (process env over a trusted ./.uagent/.config over ~/.uagent/.config —
// see core/config.h):
//   OPENROUTER_API_KEY  zero-config OpenRouter (optional model/effort
//   overrides) UAGENT_BASE_URL   e.g. http://localhost:8080/v1 UAGENT_MODEL
//   model name (unset = ask the server what it serves) UAGENT_API_KEY    any
//   string; local servers ignore it (default sk-noop) UAGENT_PROVIDERS optional
//   named provider/model JSON catalog UAGENT_REASONING_EFFORT  optional
//   none/minimal/low/medium/high/xhigh/max

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/api.h"
#include "include/cli.h"
#include "include/core/config.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/signals.h"
#include "include/core/skills.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp/register.h"
#include "include/mcp/server.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/jobs.h"
#include "include/tools/process.h"
#include "include/tools/registry.h"
#include "include/tools/shell.h"
#include "include/tools/skill.h"
#include "include/tools/subagent.h"
#include "include/tools/tool.h"
#include "include/tools/web_search.h"
#include "include/ui/display.h"
#include "include/ui/sessions.h"

namespace uagent {

class CurlRuntime {
 public:
  CurlRuntime() : ready_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
  ~CurlRuntime() {
    if (ready_) curl_global_cleanup();
  }
  bool Ready() const { return ready_; }

 private:
  bool ready_;
};

// One explicit owner for state whose lifetime spans a CLI session. Tools and
// Agent receive references to these components; none of them rely on hidden
// header-level process state for MCP, background jobs, or side-request usage.
struct AppRuntime {
  explicit AppRuntime(RuntimeConfig parsed)
      : config(std::move(parsed)), api(config) {}

  RuntimeConfig config;
  Api api;
  ProcessSupervisor processes;
  UsageAccumulator side_usage;
  SideTaskSupervisor side_tasks;
  McpRuntime mcp;
};

int Main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--log-pump") {
    char* end = nullptr;
    int64_t bytes = strtol(argv[3], &end, 10);
    return end && *end == '\0' && bytes >= 1024 ? ToolLogPump(argv[2], bytes)
                                                : 2;
  }
  std::setlocale(LC_CTYPE, "");
  g_tty = isatty(1);
  g_signal_tty = g_tty;
  signal(SIGINT, SigintHandler);
  signal(SIGTERM, SigintHandler);
  signal(SIGHUP, SigintHandler);
  signal(SIGPIPE, SIG_IGN);
#if defined(HAVE_EDITLINE)
  rl_getc_function = EscGetc;
#endif
  bool yolo = false;
  bool debug = false;
  bool trust_project = false;
  json trusted_project_config = nullptr;
  std::string debug_path;
  std::string prompt;
  std::vector<std::string> attach_paths;
  bool resume_latest = false, resume_pick = false;
  g_argv0 = argv[0];
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--yolo") {
      yolo = true;
    } else if (a == "-p" && i + 1 < argc) {
      prompt = argv[++i];
    } else if (a == "--attach" && i + 1 < argc) {
      attach_paths.push_back(argv[++i]);
    } else if (a == "--continue" || a == "-c") {
      resume_latest = true;
    } else if (a == "--resume") {
      resume_pick = true;
    } else if (a == "--trust-project-config") {
      trust_project = true;
    } else if (a == "--debug") {
      debug = true;
    } else if (a == "--version") {
      printf("uagent %s\n", kVersion);
      return 0;
    } else if (a.starts_with("--debug=")) {
      debug = true;
      debug_path = a.substr(8);
    } else if (a == "-h" || a == "--help") {
      printf(
          "usage: uagent [--yolo] [--trust-project-config] [--debug[=PATH]] "
          "[-p PROMPT] [--attach PATH] [-c] [--resume]\n\n"
          "  -p PROMPT   run one turn, print only the final answer, exit\n"
          "  --attach PATH  send an image or document with the first message\n"
          "  -c          resume the most recent saved session\n"
          "  --resume    pick a saved session to resume at startup\n"
          "  --version   print the installed version\n"
          "  --trust-project-config  allow this workspace's .mcp.json and "
          ".uagent/.config\n\n"
          "config: ./.uagent/.config when trusted, then ~/.uagent/.config; "
          "process UAGENT_* variables override both\n");
      return 0;
    } else {
      fprintf(stderr, "unknown flag: %s\n", a.c_str());
      return 2;
    }
  }

  // Project MCP configuration is executable code, and a project .uagent/.config
  // can redirect every request. Trust comes only from an inherited
  // flag/environment value, a matching stored fingerprint, or an explicit
  // interactive decision. Project files cannot grant themselves.
  trust_project = trust_project || EnvStr("UAGENT_TRUST_PROJECT_CONFIG") == "1";
  if (!trust_project) {
    trust_project = ProjectConfigTrusted(&trusted_project_config);
  }
  if (ProjectConfigPresent() && !trust_project) {
    std::string surfaces =
        ProjectMcpPresent()
            ? (ProjectAgentConfigPresent() ? ".mcp.json and .uagent/.config"
                                           : ".mcp.json")
            : ".uagent/.config";
    if (!isatty(STDIN_FILENO) || !prompt.empty()) {
      // MCP servers have nothing to degrade to, so an untrusted .mcp.json is
      // fatal. An untrusted .uagent/.config does: ~/.uagent/.config still
      // applies, so a script keeps working instead of failing on a checkout.
      if (ProjectMcpPresent()) {
        fprintf(stderr,
                "project .mcp.json is untrusted; rerun with "
                "--trust-project-config after reviewing it\n");
        return 2;
      }
      fprintf(stderr,
              "project .uagent/.config is untrusted and was ignored; rerun "
              "with --trust-project-config after reviewing it\n");
    } else {
      bool eof = false;
      std::string answer = Trim(
          ReadInputLine("Trust this workspace's " + surfaces + "? [y/N] ", &eof,
                        /*keep_history=*/false));
      trust_project =
          !eof && (answer == "y" || answer == "Y" || answer == "yes");
      if (trust_project) {
        std::string error;
        if (!TrustProjectConfig(error, &trusted_project_config)) {
          fprintf(stderr, "cannot save project trust: %s\n", error.c_str());
          return 1;
        }
      }
    }
  }
  // Explicit trust authorizes exactly this parsed snapshot. Registration
  // consumes it directly, so a file swap after approval cannot change which
  // commands are spawned.
  if (ProjectMcpPresent() && trust_project &&
      trusted_project_config.is_null()) {
    std::string error;
    if (!ProjectMcpSnapshot(trusted_project_config, error)) {
      fprintf(stderr, "cannot load trusted project config: %s\n",
              error.c_str());
      return 1;
    }
  }

  LoadConfigFile(trust_project);
  MaintainArtifacts();
  if (!yolo) yolo = EnvStr("UAGENT_APPROVAL") == "yolo";
  if (!debug) {
    debug_path = EnvStr("UAGENT_DEBUG_LOG");
    debug = !debug_path.empty();
  }
  // Headless: everything the interactive agent prints — banner, MCP status,
  // the stream, the tool trace, the footer — goes to /dev/null, and fd 1 is
  // restored at the end to carry the answer alone. fd 2 still reports errors.
  int saved_stdout = -1;
  if (!prompt.empty()) {
    fflush(stdout);
    saved_stdout = dup(1);
    fcntl(saved_stdout, F_SETFD, FD_CLOEXEC);
    int null = open("/dev/null", O_WRONLY);
    dup2(null, 1);
    close(null);
  }

  if (debug && !g_debug.Start(debug_path)) {
    fprintf(stderr, "cannot open debug log: %s\n", g_debug.Error().c_str());
    return 1;
  }
  if (g_debug.Enabled()) {
    printf("%s· debug log: %s%s\n", DIM(), g_debug.Path().c_str(), RST());
    g_debug.Write("process_start",
                  {{"pid", getpid()},
                   {"cwd", std::filesystem::current_path().string()},
                   {"tty", g_tty}});
  }

  CurlRuntime curl;
  if (!curl.Ready()) {
    fprintf(stderr, "cannot initialize libcurl\n");
    return 1;
  }

  RuntimeConfig parsed_config = RuntimeConfig::FromEnvironment();
  if (!parsed_config.web_search_effort.empty() &&
      !ValidEffort(parsed_config.web_search_effort)) {
    fprintf(stderr, "%signoring invalid UAGENT_WEB_SEARCH_EFFORT=%s%s\n", YEL(),
            TerminalSafe(parsed_config.web_search_effort).c_str(), RST());
    parsed_config.web_search_effort.clear();
  }
  AppRuntime runtime(std::move(parsed_config));
  RuntimeConfig& runtime_config = runtime.config;
  Api& api = runtime.api;
  ProjectInstructions project_instructions = LoadProjectInstructions(
      CanonicalAccessPath(CanonicalCwd()),
      static_cast<size_t>(runtime_config.project_doc_bytes));
  // Name what was loaded into the first message: instruction files carry
  // standing orders and memories carry what earlier sessions concluded, so it
  // should never be a mystery which of them are in play.
  if (!project_instructions.sources.empty()) {
    std::string cwd = CanonicalCwd() + "/";
    std::string list;
    for (const std::string& source : project_instructions.sources) {
      if (!list.empty()) list += ", ";
      list +=
          source.starts_with(cwd) ? source.substr(cwd.size()) : Tilde(source);
    }
    printf("%s· context: %s%s\n", DIM(), TerminalSafe(list).c_str(), RST());
  }
  std::vector<Skill> skills = LoadSkills(CanonicalCwd());
  if (!skills.empty()) {
    // Descriptions ride every request, so the catalogue has a standing price.
    // Report it rather than let it accumulate invisibly as skills pile up.
    std::string list;
    size_t bytes = 0;
    for (const Skill& skill : skills) {
      if (!list.empty()) list += ", ";
      list += skill.name;
      bytes += skill.name.size() + skill.description.size();
    }
    std::string summary = std::to_string(skills.size()) + ", ~" +
                          FmtTokens(static_cast<int64_t>(bytes) / 4) +
                          " tokens/request — " + list;
    printf("%s· skills: %s%s\n", DIM(),
           TerminalFit(TerminalSafe(summary), "· skills: ").c_str(), RST());
  }
  if (project_instructions.truncated) {
    std::cerr << YEL() << "project instructions truncated at "
              << runtime_config.project_doc_bytes << " bytes" << RST() << '\n';
  }
  ProviderSetup provider = ConfigureProvider(api);
  std::vector<ModelRoute>& model_routes = provider.routes;
  if (!provider.warning.empty()) {
    fprintf(stderr, "%s%s%s\n", YEL(), TerminalSafe(provider.warning).c_str(),
            RST());
  }
#if defined(HAVE_EDITLINE)
  ConfigureReadlineCompletion(model_routes);
#endif
  if (api.base_url.empty()) {
    DebugLog("startup_error", {{"error", "UAGENT_BASE_URL is not set"}});
    fprintf(stderr,
            "%sno provider configured%s — set OPENROUTER_API_KEY or point "
            "UAGENT_BASE_URL at an OpenAI-compatible endpoint, e.g.\n"
            "  export UAGENT_BASE_URL=http://localhost:8080/v1\n",
            RED(), RST());
    return 1;
  }

  if (api.model.empty() ||
      (api.ctx_window == 0 && !OpenrouterUrl(api.base_url))) {
    auto probe_started = std::chrono::steady_clock::now();
    json models = api.Get("/models");
    double probe_ms = ElapsedMs(probe_started);
    size_t offered = 0;
    if (models.is_object() && models.contains("data") &&
        models["data"].is_array()) {
      const json& data = models["data"];
      offered = data.size();
      if (api.model.empty()) {
        for (const json& candidate : data) {
          if (candidate.is_object()) {
            api.model = JsonValue(candidate, "id", "");
            if (!api.model.empty()) break;
          }
        }
      }
      // ":online"-style routing suffixes are not separate /models entries
      std::string base = api.model.substr(0, api.model.find(':'));
      if (api.ctx_window == 0) {
        for (const json& m : data) {
          if (!m.is_object()) continue;
          std::string id = JsonString(m, "id");
          if (id != api.model && id != base) continue;
          api.ctx_window = JsonInt(m, "context_length");  // OpenRouter
          if (!api.ctx_window) {
            api.ctx_window = JsonInt(m, "max_model_len");  // vLLM
          }
          if (!api.ctx_window && m.contains("meta") && m["meta"].is_object()) {
            api.ctx_window = JsonInt(m["meta"], "n_ctx_train");  // llama.cpp
          }
          break;
        }
      }
    }
    // The catalog itself is never logged: on OpenRouter it is ~530 KB of
    // models we do not use, which would dwarf the rest of the trace.
    DebugLog("models_probe", {{"duration_ms", probe_ms},
                              {"models_offered", offered},
                              {"model", api.model},
                              {"context_window", api.ctx_window}});
    if (api.model.empty()) {
      DebugLog("startup_error",
               {{"error", "no usable model"}, {"base_url", api.base_url}});
      fprintf(
          stderr,
          "%sUAGENT_MODEL is not set and %s/models returned nothing usable%s\n",
          RED(), api.base_url.c_str(), RST());
      return 1;
    }
  }

  auto approver = [&yolo](const Tool& t, const json& args) {
    bool granted = true;
    if (!yolo) {
      std::string q = std::string(YEL()) + "allow " + TerminalSafe(t.name) +
                      ": " + TerminalSafe(OneLine(ToolSummary(t, args), 120)) +
                      "? [Y/n] " + RST();
      bool eof = false;
      std::string ans = Trim(ReadInputLine(q, &eof, /*keep_history=*/false));
      granted =
          !eof && (ans.empty() || ans == "y" || ans == "Y" || ans == "yes");
    }
    DebugLog("approval",
             {{"tool", t.name}, {"automatic", yolo}, {"granted", granted}});
    return granted;
  };

  printf("%sµAgent%s\n", BOLD(), RST());

  ProcessSupervisor& processes = runtime.processes;
  UsageAccumulator& side_usage = runtime.side_usage;
  SideTaskSupervisor& side_tasks = runtime.side_tasks;
  McpRuntime& mcp = runtime.mcp;
  bool inline_images =
      prompt.empty() && g_tty &&
      DetectTerminalImageProtocol() != TerminalImageProtocol::kNone;
  std::vector<Tool> tools =
      BuiltinTools(processes, CanonicalAccessPath(CanonicalCwd()),
                   inline_images, &side_tasks);
  bool has_openrouter = OpenrouterUrl(api.base_url) ||
                        std::any_of(model_routes.begin(), model_routes.end(),
                                    [](const ModelRoute& route) {
                                      return OpenrouterUrl(route.base_url);
                                    });
  if (has_openrouter) {
    tools.push_back(
        WebSearchTool(api, side_usage, side_tasks));  // billed side-request
  }
  McpRegister(
      tools, mcp, runtime_config,
      trusted_project_config);  // immutable trusted snapshot + user servers
  if (CanDelegate()) {  // subagents delegate too, up to UAGENT_SUBAGENT_DEPTH
    tools.push_back(SubagentTool(api, processes, yolo, debug));
  }
  if (!skills.empty()) tools.push_back(SkillTool(std::move(skills)));
  Agent agent(
      api, tools, processes, side_tasks, side_usage, approver,
      [&](std::chrono::steady_clock::time_point deadline) {
        return McpRefreshTools(tools, mcp, runtime_config, deadline);
      },
      std::move(project_instructions));
  if (g_debug.Enabled()) {
    g_debug.Write(
        "session_ready",
        {{"base_url", api.base_url},
         {"model", api.model},
         {"reasoning_effort", api.reasoning_effort},
         {"configured_models", model_routes.size()},
         {"context_window", api.ctx_window},
         {"tools", tools.size()},
         {"yolo", yolo},
         {"auto_compact_pct", AutoCompactPct()},
         {"checkpoint_mode", runtime_config.checkpoint_mode},
         {"checkpoint_pct", CheckpointPct()},
         {"checkpoint_urgent_pct", CheckpointUrgentPct()},
         {"openrouter_provider", runtime_config.openrouter_provider},
         {"openrouter_fallbacks", runtime_config.openrouter_fallbacks},
         {"tool_concurrency", ToolConcurrency()},
         {"tool_result_chars", ToolResultCap()},
         {"attachment_mb", AttachmentLimitMb()},
         {"image_detail", ImageDetail()},
         {"steering", SteeringEnabled()},
         {"max_tokens", MaxOutputTokens()},
         {"limits", runtime_config.DiagnosticJson()}});
  }

  std::vector<Attachment> attachments;
  for (const std::string& path :
       attach_paths) {  // --attach: ride on message one
    Attachment attachment;
    std::string error;
    if (!InspectAttachment(path, attachment, error)) {
      fprintf(stderr, "%s\n", error.c_str());
      return 2;
    }
    attachments.push_back(std::move(attachment));
  }
  auto run_turns = [&](std::string input, json content = nullptr) {
    bool resume = false;
    for (;;) {
      if (resume) {
        agent.Resume();
      } else {
        agent.Turn(input, std::move(content));
      }
      if (!g_steering.Take()) break;
      bool cancelled = false;
      std::string next = SteeringReplacement(cancelled);
      if (cancelled) {
        printf("%s· resuming%s\n", DIM(), RST());
        resume = true;
        content = nullptr;
        continue;
      }
      printf("%s· applying steering%s\n", DIM(), RST());
      input = std::move(next);
      content = nullptr;
      resume = false;
    }
  };

  if (!prompt.empty()) {  // headless: run one turn, report only the answer
    json content;
    if (!attachments.empty()) {
      std::string error;
      content = AttachmentContent(prompt, attachments, error);
      if (!error.empty()) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
      }
    }
    run_turns(prompt, std::move(content));
    fflush(stdout);
    dup2(saved_stdout, 1);
    close(saved_stdout);
    std::string ledger = EnvStr("UAGENT_USAGE_FILE");
    if (!ledger.empty()) {  // report the spend so the parent can bill it
      std::string error;
      if (!AppendPrivateLine(ledger, JsonDump(UsageJson(agent.SessionUsage())),
                             error)) {
        fprintf(stderr, "cannot write usage ledger: %s\n", error.c_str());
      }
    }
    std::string answer = agent.LastText();
    BgShutdownAll(processes);
    if (answer
            .empty()) {  // a caller reading stdout must not see silent failure
      std::string error = agent.LastError().empty()
                              ? "agent produced no answer"
                              : TerminalSafe(agent.LastError());
      if (g_debug.Enabled()) {
        g_debug.Write("session_end",
                      {{"reason", "headless_error"},
                       {"usage", UsageJson(agent.SessionUsage())},
                       {"context_tokens", agent.ContextUsed()}});
      }
      fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    if (g_debug.Enabled()) {
      g_debug.Write("session_end", {{"reason", "headless_complete"},
                                    {"usage", UsageJson(agent.SessionUsage())},
                                    {"context_tokens", agent.ContextUsed()}});
    }
    printf("%s\n", TerminalSafe(answer).c_str());
    return 0;
  }

  // current session's save file ("" until the first turn mints one)
  std::string session_file;
  if (resume_pick) {
    ResumeInto(agent, PickSession(), session_file);
  } else if (resume_latest) {
    std::vector<SessionInfo> s = ListSessions();
    if (s.empty()) {
      printf("%s· no saved sessions%s\n", DIM(), RST());
    } else {
      ResumeInto(agent, s.front().path, session_file);
    }
  }

  // Flush the conversation after anything that changed it — top of the loop
  // (after the previous turn) and once at exit. `saved_msgs` starts from the
  // post-resume count so a just-loaded session isn't rewritten before use.
  // Only real interactive sessions are kept; a piped `echo q | uagent` (or a
  // subagent, which pipes too) is a one-off, not something to resume.
  bool persist = isatty(0);
  uint64_t saved_revision = agent.Revision();
  auto save_session = [&] {
    if (!persist || agent.MessageCount() <= 1 ||
        agent.Revision() == saved_revision) {
      return;
    }
    if (session_file.empty()) {
      session_file = UagentDir("history") + "/" + WorkspaceId(CanonicalCwd()) +
                     "/" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
                     std::to_string(getpid()) + ".json";
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(session_file).parent_path(), ec);
    chmod(std::filesystem::path(session_file).parent_path().c_str(), 0700);
    std::string error;
    if (!agent.Save(session_file, error)) {
      fprintf(stderr, "cannot save session: %s\n", error.c_str());
      return;
    }
    saved_revision = agent.Revision();
  };

  std::string exit_reason = "eof";
  for (;;) {
    save_session();
    agent.DrainBackground();
    PrintStatusBar(StatusBar(api, agent, yolo, attachments.size(), processes));
    PanelClearLine();
    bool eof = false;
    std::string line = ReadInputLine(PanelPrompt(), &eof);
    if (eof) {
      if (g_tty) printf("\r\033[2K\r");
      printf("\n");
      break;
    }
    std::string input = Trim(line);
    if (input.empty()) continue;

    if (input[0] == '/') DebugLog("command", {{"command", input}});

    ParsedSlashCommand command = ParseSlashCommand(input);
    if (command.spec) {
      bool quit = false;
      switch (command.spec->id) {
        case SlashCommandId::kQuit:
          exit_reason = "command";
          quit = true;
          break;
        case SlashCommandId::kReset:
          agent.Reset();
          attachments.clear();
          session_file.clear();  // the old session file stays
          saved_revision = agent.Revision();
          printf("%s· fresh session%s\n", DIM(), RST());
          break;
        case SlashCommandId::kSessions: {
          std::string chosen = PickSession();
          if (!chosen.empty()) {
            ResumeInto(agent, chosen, session_file);
            attachments.clear();
            saved_revision = agent.Revision();
          }
          break;
        }
        case SlashCommandId::kModels:
          if (command.argument.empty()) {
            PrintModelRoutes(model_routes, api);
            printf(
                "%s· /models all for the live catalog; "
                "/models FILTER to search%s\n",
                DIM(), RST());
          } else {
            PrintAvailableModels(api, command.argument);
          }
          break;
        case SlashCommandId::kModel: {
          if (command.argument.empty()) {
            PrintModelRoutes(model_routes, api);
            break;
          }
          std::string selected =
              SelectModel(api, model_routes, command.argument);
          if (selected.empty()) {
            printf("%s· unknown model %s; use /models%s\n", RED(),
                   TerminalSafe(command.argument).c_str(), RST());
            break;
          }
          bool named_route = FindModelRoute(model_routes, selected) != nullptr;
          std::string preference_error;
          bool preference_saved = SaveModelPreference(
              {selected, api.base_url, named_route}, preference_error);
          agent.RouteChanged();
          DebugLog("route_changed", {{"route", selected},
                                     {"model", api.model},
                                     {"base_url", api.base_url},
                                     {"effort", api.reasoning_effort},
                                     {"preference_saved", preference_saved}});
          printf("%s· model %s · effort %s%s\n", DIM(), selected.c_str(),
                 api.reasoning_effort.empty() ? "default"
                                              : api.reasoning_effort.c_str(),
                 RST());
          if (!preference_saved) {
            printf("%s· model changed but preference was not saved: %s%s\n",
                   YEL(), TerminalSafe(preference_error).c_str(), RST());
          }
          break;
        }
        case SlashCommandId::kEffort:
          if (command.argument.empty()) {
            printf("%s· effort %s%s\n", DIM(),
                   api.reasoning_effort.empty() ? "default"
                                                : api.reasoning_effort.c_str(),
                   RST());
          } else if (command.argument == "default") {
            api.reasoning_effort.clear();
            setenv("UAGENT_REASONING_EFFORT", "", 1);
            agent.RouteChanged();
            printf("%s· effort provider default%s\n", DIM(), RST());
          } else if (!ValidEffort(command.argument)) {
            printf(
                "%s· effort must be none, minimal, low, medium, high, xhigh, "
                "or "
                "max; use default to defer to the provider%s\n",
                RED(), RST());
          } else {
            api.reasoning_effort = command.argument;
            setenv("UAGENT_REASONING_EFFORT", command.argument.c_str(), 1);
            agent.RouteChanged();
            printf("%s· effort %s%s\n", DIM(), command.argument.c_str(), RST());
          }
          break;
        case SlashCommandId::kYolo:
          yolo = !yolo;
          printf("%s· yolo %s%s\n", DIM(),
                 yolo ? "ON — auto-approving everything" : "off", RST());
          break;
        case SlashCommandId::kCompact:
          for (;;) {
            agent.Compact();
            if (!g_steering.Take()) break;
            bool cancelled = false;
            std::string next = SteeringReplacement(cancelled);
            if (cancelled) {
              printf("%s· resuming%s\n", DIM(), RST());
              continue;
            }
            run_turns(std::move(next));
            break;
          }
          break;
        case SlashCommandId::kAttach:
          if (command.argument.empty()) {
            if (attachments.empty()) {
              printf("%s· no pending attachments%s\n", DIM(), RST());
            } else {
              for (const Attachment& attachment : attachments) {
                printf("%s· %s (%s)%s\n", DIM(),
                       TerminalSafe(attachment.path).c_str(),
                       attachment.mime.c_str(), RST());
              }
            }
          } else {
            Attachment attachment;
            std::string error;
            if (!InspectAttachment(command.argument, attachment, error)) {
              printf("%s%s%s\n", RED(), error.c_str(), RST());
            } else {
              attachments.push_back(std::move(attachment));
              printf("%s· attached %s for the next message%s\n", DIM(),
                     attachments.back().name.c_str(), RST());
            }
          }
          break;
        case SlashCommandId::kDetach:
          attachments.clear();
          printf("%s· attachments cleared%s\n", DIM(), RST());
          break;
        case SlashCommandId::kOnline: {  // OpenRouter web-search suffix
          if (!OpenrouterUrl(api.base_url)) {
            printf("%s· /online is available only for OpenRouter%s\n", RED(),
                   RST());
            break;
          }
          bool on = api.model.size() > 7 &&
                    api.model.compare(api.model.size() - 7, 7, ":online") == 0;
          if (on) {
            api.model.erase(api.model.size() - 7);
          } else {
            api.model += ":online";
          }
          setenv("UAGENT_MODEL", api.model.c_str(), 1);
          printf("%s· web search %s%s\n", DIM(),
                 on ? "off"
                    : "ON — ~2K extra input tokens + search fees per request",
                 RST());
          break;
        }
      }
      if (quit) break;
      continue;
    }
    if (input[0] == '/') {
      PrintCommandHelp();
      continue;
    }

    json content;
    if (!attachments.empty()) {
      std::string error;
      content = AttachmentContent(input, attachments, error);
      if (!error.empty()) {
        printf("%s%s%s\n", RED(), error.c_str(), RST());
        continue;
      }
      attachments.clear();
    }
    run_turns(input, std::move(content));
  }

  save_session();  // final flush: the last turn, or a /compact with no
                   // follow-up
  BgShutdownAll(processes);
  std::remove(UsageLedger().c_str());  // a recycled pid must not inherit it

  if (g_debug.Enabled()) {
    const Usage& usage = agent.SessionUsage();
    g_debug.Write("session_end", {{"reason", exit_reason},
                                  {"usage", UsageJson(usage)},
                                  {"context_tokens", agent.ContextUsed()}});
  }
  TerminalRestore();
  return 0;
}

}  // namespace uagent

int main(int argc, char** argv) { return uagent::Main(argc, argv); }
