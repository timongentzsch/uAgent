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
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools.h"

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

// Delegation: re-invoke this same binary on a scoped sub-task. The child's
// reasoning and tool trace stay in its own context and its own log; only the
// final answer comes back, so a wide search costs the coordinator a paragraph
// instead of fifty tool results. The shell runner does the rest — a quick
// sub-task answers inline, a slow one backgrounds itself and is collected by
// pid.
static Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                         bool yolo, bool debug) {
  std::string self = g_argv0;
  std::string child_depth = std::to_string(AgentDepth() + 1);
  Tool t = MakeTool(
      "task",
      "Delegate substantial independent research or analysis. The child sees "
      "no "
      "conversation, so include every path and constraint.",
      json::parse(R"json({"type":"object","properties":{
          "prompt":{"type":"string","description":"complete standalone brief"}},
          "required":["prompt"]})json"),
      [self, child_depth, &api, yolo, debug, &processes](
          const json& a, const ToolContext& context) {
        std::string cmd =
            "UAGENT_DEPTH=" + child_depth +
            " UAGENT_MAX_STEPS=" + std::to_string(SubagentMaxSteps()) +
            " UAGENT_MAX_TOOL_CALLS=" + std::to_string(SubagentMaxToolCalls()) +
            " UAGENT_MODEL=" + ShellQuote(api.model) +
            " UAGENT_CONTEXT=" + std::to_string(api.ctx_window) +
            " UAGENT_REASONING_EFFORT=" + ShellQuote(api.reasoning_effort) +
            " UAGENT_USAGE_FILE=" + ShellQuote(UsageLedger()) + " " +
            ShellQuote(self) + (yolo ? " --yolo" : "") +
            (debug ? " --debug" : "") + " -p " +
            ShellQuote(JsonValue(a, "prompt", ""));
        return ToolRunBash(processes, cmd, context.timeout_s,
                           /*join_before_final=*/true, context);
      });
  t.mutating = true;
  t.summary = [](const json& a) { return JsonValue(a, "prompt", ""); };
  t.timeout_s = 3;
  // Delegation only overlaps if each spawn backgrounds fast. A model-supplied
  // `timeout` must not stretch the foreground window and serialise the fleet.
  t.max_timeout_s = t.timeout_s;
  return t;  // not parallel_safe: process spawning and sync cancellation are
}  // single-slot, so spawns serialise — the children still overlap

// OpenRouter-only: lets the model reach the web when IT decides it needs to,
// via a quiet side-request to <model>:online. Costs one search-enabled
// completion per call — but only when actually used, unlike the /online
// toggle which pays on every request.
static Tool WebSearchTool(Api& api, UsageAccumulator& usage,
                          SideTaskSupervisor& side_tasks) {
  Tool t = MakeTool(
      "web_search",
      "Search via OpenRouter with source URLs. Batch up to four queries. Slow "
      "searches "
      "background automatically; continue useful work and join only when "
      "needed. "
      "Do not repeat.",
      json::parse(R"json({"type":"object","properties":{
          "query":{"type":"string","description":"one query (legacy shorthand)"},
          "queries":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":4,
            "description":"one to four queries in one request"}}})json"),
      [&api, &usage, &side_tasks](const json& a,
                                  const ToolContext& context) -> std::string {
        if (!OpenrouterUrl(api.base_url)) {
          return "error: web_search is available only for OpenRouter";
        }
        std::vector<std::string> queries;
        if (a.contains("queries") && a["queries"].is_array()) {
          for (const json& value : a["queries"]) {
            if (value.is_string() && !Trim(value.get<std::string>()).empty()) {
              queries.push_back(Trim(value.get<std::string>()));
            }
          }
        }
        if (queries.empty()) {
          std::string query = Trim(JsonValue(a, "query", ""));
          if (!query.empty()) queries.push_back(std::move(query));
        }
        if (queries.empty()) return "error: query or queries is required";
        if (queries.size() > 4) {
          return "error: queries is limited to four items";
        }
        std::string query;
        for (size_t i = 0; i < queries.size(); ++i) {
          query += std::to_string(i + 1) + ". " + queries[i] + "\n";
        }
        const std::string base_url = api.base_url, api_key = api.api_key;
        const std::string model = api.model;
        RuntimeConfig config = api.config;
        std::string base =
            config.web_search_model.empty() ? model : config.web_search_model;
        base = base.substr(0, base.find(':'));
        json body = {
            {"model", base + ":online"},
            {"max_tokens", config.web_search_max_tokens},
            {"usage", {{"include", true}}},  // OpenRouter: report cost
            {"messages",
             json::array(
                 {{{"role", "user"},
                   {"content",
                    "Answer each numbered query concisely with source URLs. "
                    "Use only source-supported claims; preserve provider/model "
                    "scope and omit unasked pricing:\n" +
                        query}}})}};
        if (!config.web_search_effort.empty()) {
          body["reasoning"] = {{"effort", config.web_search_effort}};
        }
        int64_t timeout =
            std::max(config.web_search_timeout_s, context.timeout_s);
        int64_t id = side_tasks.Start(
            "web search", query,
            [base_url, api_key, config, body = std::move(body), timeout,
             &usage](const std::atomic<bool>& cancel) {
              auto started = std::chrono::steady_clock::now();
              DebugLog("side_request", {{"kind", "web_search"},
                                        {"path", "/chat/completions"},
                                        {"body", body}});
              Api side(config);
              side.base_url = base_url;
              side.api_key = api_key;
              json r = side.Post("/chat/completions", body, timeout, &cancel);
              DebugLog("side_response",
                       {{"kind", "web_search"},
                        {"duration_ms", ElapsedMs(started)},
                        {"cancelled", cancel.load()},
                        {"response", r.is_discarded() ? json(nullptr) : r}});
              if (cancel.load()) {
                return std::string("error: web search abandoned");
              }
              if (AbortRequested()) {
                return std::string("error: search cancelled by user");
              }
              if (r.is_object() && r.contains("usage")) usage.Add(r["usage"]);
              if (r.is_object() && r.contains("choices") &&
                  r["choices"].is_array() && !r["choices"].empty() &&
                  r["choices"][0].is_object()) {
                const json& choice = r["choices"][0];
                std::string content = "(empty answer)";
                if (choice.contains("message") &&
                    choice["message"].is_object()) {
                  content = JsonString(choice["message"], "content", content);
                }
                std::string output =
                    "[web search result; refetch only if verification is "
                    "necessary]\n" +
                    content;
                if (JsonString(choice, "finish_reason") == "length") {
                  output += "\n[truncated; raise UAGENT_WEB_SEARCH_MAX_TOKENS]";
                }
                return output;
              }
              if (r.is_object() && r.contains("error") &&
                  r["error"].is_object()) {
                return "error: " +
                       JsonString(r["error"], "message", "search failed");
              }
              return std::string("error: web search failed");
            },
            ToolConcurrency());
        if (!id) return "error: concurrent side-task limit reached";
        int64_t grace = context.timeout_s == 0
                            ? timeout
                            : std::min(context.timeout_s, timeout);
        if (auto result = side_tasks.Wait(id, std::chrono::seconds(grace))) {
          return result->output;
        }
        DebugLog("side_backgrounded",
                 {{"kind", "web_search"}, {"id", id}, {"query", query}});
        return "[backgrounded] web search job " + std::to_string(id) +
               "; continue other work or call wait_background(id=" +
               std::to_string(id) + ")";
      });
  t.mutating = true;
  t.summary = [](const json& a) {
    if (a.contains("queries") && a["queries"].is_array()) {
      std::string summary;
      for (const json& query : a["queries"]) {
        if (query.is_string()) {
          summary += (summary.empty() ? "" : " | ") +
                     OneLine(query.get<std::string>(), 60);
        }
      }
      return summary;
    }
    return JsonValue(a, "query", "");
  };
  t.parallel_safe = true;
  t.timeout_s = 5;
  t.max_calls_per_turn = api.config.web_search_calls;
  return t;
}

// --- saved sessions ---------------------------------------------------------
// One file per conversation under ~/.uagent/history, written by Agent::save as
// two lines: a cheap header (read here for the picker) and the full payload.

struct SessionInfo {
  std::string path, cwd, title;
  int64_t turns = 0;
  std::filesystem::file_time_type mtime;
};

// abbreviate $HOME to ~ for display
static std::string Tilde(const std::string& path) {
  std::string home = UserHome();
  if (!home.empty() && path.starts_with(home)) {
    return "~" + path.substr(home.size());
  }
  return path;
}

static std::string ApiHost(const std::string& base_url) {
  return UrlHost(base_url);
}

#if defined(HAVE_EDITLINE)
static void ConfigureReadlineCompletion(const std::vector<ModelRoute>& routes) {
  std::vector<std::string> models, efforts{"default"};
  for (const ModelRoute& route : routes) models.push_back(route.name);
  efforts.insert(efforts.end(), std::begin(kReasoningEfforts),
                 std::end(kReasoningEfforts));
  ConfigureCompletion(models, efforts);
}
#endif

static void PrintModelRoutes(const std::vector<ModelRoute>& routes,
                             const Api& api) {
  if (routes.empty()) {
    printf("%s* %-20s %s @ %s%s\n", BOLD(), api.model.c_str(),
           api.model.c_str(), ApiHost(api.base_url).c_str(), RST());
    return;
  }
  for (const ModelRoute& route : routes) {
    bool active = route.base_url == api.base_url && route.model == api.model &&
                  route.effort == api.reasoning_effort;
    printf("%s%c %-20s %s", active ? BOLD() : DIM(), active ? '*' : ' ',
           route.name.c_str(), route.model.c_str());
    if (!route.effort.empty()) printf(" · %s", route.effort.c_str());
    printf(" @ %s%s\n", ApiHost(route.base_url).c_str(), RST());
  }
}

static void PrintAvailableModels(Api& api, std::string filter) {
  printf("%s· querying %s/models…%s\n", DIM(), ApiHost(api.base_url).c_str(),
         RST());
  std::optional<std::vector<ModelInfo>> models =
      QueryModels(api, std::move(filter));
  if (!models) {
    printf("%s· model catalog unavailable%s\n", RED(), RST());
    return;
  }
  for (const ModelInfo& model : *models) {
    bool active = model.id == api.model;
#if defined(HAVE_EDITLINE)
    RegisterCompletion(CommandCompletion::kModels, model.id);
#endif
    if (active && model.context > 0) {
      api.ctx_window = model.context;
      setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
    }
    printf("%s%c %s", active ? BOLD() : DIM(), active ? '*' : ' ',
           model.id.c_str());
    if (model.context > 0) {
      printf(" · ctx %s", FmtTokens(model.context).c_str());
    }
    if (!model.efforts.empty()) {
      printf(" · effort ");
      for (size_t i = 0; i < model.efforts.size(); ++i) {
        printf("%s%s", i ? "," : "", model.efforts[i].c_str());
      }
      if (!model.default_effort.empty()) {
        printf(" (default %s)", model.default_effort.c_str());
      }
    }
    printf("%s\n", RST());
  }
  printf("%s· %zu model%s%s\n", DIM(), models->size(),
         models->size() == 1 ? "" : "s", RST());
}

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

// newest first; malformed files still list, with a fallback title
static std::vector<SessionInfo> ListSessions() {
  namespace fs = std::filesystem;
  std::vector<SessionInfo> out;
  std::error_code ec;
  std::string current = CanonicalCwd();
  std::string base = UagentDir("history");
  std::string scoped = base + "/" + WorkspaceId(current);
  fs::create_directories(scoped, ec);
  chmod(scoped.c_str(), 0700);
  for (const std::string& dir : {scoped, base}) {
    for (auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
      std::ifstream f(e.path());
      std::string head;
      std::getline(f, head);
      json h = json::parse(head, nullptr, false);
      if (!h.is_object() || JsonValue(h, "cwd", "") != current) continue;
      SessionInfo s;
      s.path = e.path().string();
      s.mtime = e.last_write_time(ec);
      s.cwd = JsonValue(h, "cwd", "");
      s.turns = JsonValue(h, "turns", int64_t{0});
      s.title = JsonValue(h, "title", "(untitled)");
      out.push_back(std::move(s));
    }
  }
  std::sort(out.begin(), out.end(),
            [](const SessionInfo& a, const SessionInfo& b) {
              return a.mtime > b.mtime;
            });
  return out;
}

// print a numbered list and read a choice; returns the chosen path or "".
static std::string PickSession() {
  std::vector<SessionInfo> sessions = ListSessions();
  if (sessions.empty()) {
    printf("%s· no saved sessions%s\n", DIM(), RST());
    return "";
  }
  auto now = std::filesystem::file_time_type::clock::now();
  size_t shown = std::min(sessions.size(), size_t{20});
  for (size_t i = 0; i < shown; ++i) {
    const SessionInfo& s = sessions[i];
    int64_t secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - s.mtime).count();
    std::string safe_cwd = TerminalSafe(Tilde(s.cwd));
    std::string safe_title = TerminalSafe(OneLine(s.title, 60));
    std::cout << CYAN() << '[' << i + 1 << ']' << RST() << ' ' << FmtAgo(secs)
              << " · " << s.turns << " turn" << (s.turns == 1 ? "" : "s")
              << " · " << DIM() << safe_cwd << " · \"" << safe_title << '"'
              << RST() << '\n';
  }
  bool eof = false;
  std::string ans =
      Trim(ReadInputLine("resume #: ", &eof, /*keep_history=*/false));
  if (eof || ans.empty()) return "";
  char* end = nullptr;
  int64_t n = strtol(ans.c_str(), &end, 10);
  if (end && *end == '\0' && n >= 1 && n <= static_cast<int64_t>(shown)) {
    return sessions[n - 1].path;
  }
  printf("%s· not a listed number%s\n", DIM(), RST());
  return "";
}

// load `path` into the agent; on success the session continues in that file
static void ResumeInto(Agent& agent, const std::string& path,
                       std::string& session_file) {
  if (path.empty()) return;
  std::string error;
  if (!agent.Load(path, CanonicalCwd(), error)) {
    std::string safe_path = TerminalSafe(path);
    std::string safe_error = TerminalSafe(error);
    printf("%s· could not resume %s: %s%s\n", RED(), safe_path.c_str(),
           safe_error.c_str(), RST());
    return;
  }
  session_file = path;
  printf("%s· resumed — %zu messages%s\n", DIM(), agent.MessageCount() - 1,
         RST());
  agent.PrintHistory();
  printf("%s· end of history, continuing%s\n", DIM(), RST());
}

// Prompt metadata stays in normal scrollback rather than a pinned TUI region.
static std::string StatusBar(const Api& api, const Agent& agent, bool yolo,
                             size_t attachments,
                             const ProcessSupervisor& processes) {
  const Usage& u = agent.SessionUsage();
  std::string host = ApiHost(api.base_url);
  int64_t used = agent.ContextUsed();
  std::string s = api.model + " @ " + host + " · ctx " + FmtTokens(used);
  if (api.ctx_window > 0) {
    int64_t pct = (used * 100 + api.ctx_window - 1) / api.ctx_window;
    s += "/" + FmtTokens(api.ctx_window) + " (" + std::to_string(pct) + "%)";
  }
  if (!api.reasoning_effort.empty()) s += " · effort " + api.reasoning_effort;
  if (u.cache_read) s += " · cache " + FmtTokens(u.cache_read) + " total";
  if (u.cost > 0) s += " · spent " + FmtCost(u.cost);
  if (processes.PendingCount()) {
    s += " · bg:";
    for (pid_t pid : processes.PendingPids()) s += " " + std::to_string(pid);
  }
  size_t terminals = processes.DetachedCount();
  if (terminals) s += " · terminals:" + std::to_string(terminals);
  if (yolo) s += " · YOLO";
  if (attachments) s += " · " + std::to_string(attachments) + " attached";
  return s;
}

static void PrintStatusBar(const std::string& status) {
  std::string safe = TerminalSafe(status);
  if (!g_tty) {
    printf("%s\n", safe.c_str());
    return;
  }
  safe = TerminalFit(safe);
  printf("%s%s\033[K%s\n", PanelMuted(), safe.c_str(), RST());
}

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
