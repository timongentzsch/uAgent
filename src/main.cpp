// µAgent — a lean C++ terminal coding agent for OpenAI-compatible endpoints.
//
// Config (process env over ~/.uagent/.config — see util.hpp):
//   OPENROUTER_API_KEY  zero-config OpenRouter (optional model/effort overrides)
//   UAGENT_BASE_URL   e.g. http://localhost:8080/v1
//   UAGENT_MODEL      model name (unset = ask the server what it serves)
//   UAGENT_API_KEY    any string; local servers ignore it (default sk-noop)
//   UAGENT_PROVIDERS  optional named provider/model JSON catalog
//   UAGENT_REASONING_EFFORT  optional none/minimal/low/medium/high/xhigh/max

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "agent.hpp"
#include "api.hpp"
#include "cli.hpp"
#include "media.hpp"
#include "mcp.hpp"
#include "providers.hpp"
#include "tools.hpp"
#include "util.hpp"

class CurlRuntime {
public:
    CurlRuntime() : ready_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
    ~CurlRuntime() {
        if (ready_) curl_global_cleanup();
    }
    bool ready() const { return ready_; }

private:
    bool ready_;
};

// Delegation: re-invoke this same binary on a scoped sub-task. The child's
// reasoning and tool trace stay in its own context and its own log; only the
// final answer comes back, so a wide search costs the coordinator a paragraph
// instead of fifty tool results. run_bash does the rest — a quick sub-task
// answers inline, a slow one backgrounds itself and is collected by pid.
static Tool subagent_tool(const Api& api, ProcessSupervisor& processes, bool yolo,
                          bool debug) {
    std::string self = g_argv0;
    Tool t = make_tool(
        "task",
        "Delegate self-contained research or code analysis. The child sees no conversation; "
        "include every needed path and constraint. Use only for substantial independent work.",
        json::parse(R"json({"type":"object","properties":{
          "prompt":{"type":"string","description":"complete standalone brief"}},
          "required":["prompt"]})json"),
        [self, &api, yolo, debug, &processes](const json& a,
                                             const ToolContext& context) {
            std::string cmd = "UAGENT_DEPTH=1 UAGENT_MODEL=" + shell_quote(api.model) +
                              " UAGENT_CONTEXT=" + std::to_string(api.ctx_window) +
                              " UAGENT_REASONING_EFFORT=" +
                              shell_quote(api.reasoning_effort) +
                              " UAGENT_USAGE_FILE=" + shell_quote(usage_ledger()) + " " +
                              shell_quote(self) + (yolo ? " --yolo" : "") +
                              (debug ? " --debug" : "") + " -p " +
                              shell_quote(a.value("prompt", ""));
            return tool_run_bash(processes, cmd, context.timeout_s,
                                 /*join_before_final=*/true, context);
        });
    t.mutating = true;
    t.summary = [](const json& a) { return a.value("prompt", ""); };
    t.timeout_s = 3;
    return t;  // not parallel_safe: process spawning and sync cancellation are
}              // single-slot, so spawns serialise — the children still overlap

// OpenRouter-only: lets the model reach the web when IT decides it needs to,
// via a quiet side-request to <model>:online. Costs one search-enabled
// completion per call — but only when actually used, unlike the /online
// toggle which pays on every request.
static Tool web_search_tool(Api& api, UsageAccumulator& usage,
                            SideTaskSupervisor& side_tasks) {
    Tool t = make_tool(
        "web_search",
        "OpenRouter search with source URLs. Batch independent queries; slow searches "
        "return automatically—never wait_background or repeat them.",
        json::parse(R"json({"type":"object","properties":{
          "query":{"type":"string"}},"required":["query"]})json"),
        [&api, &usage, &side_tasks](const json& a,
                                   const ToolContext& context) -> std::string {
        if (!openrouter_url(api.base_url))
            return "error: web_search is available only for OpenRouter";
        const std::string query = a.value("query", "");
        const std::string base_url = api.base_url, api_key = api.api_key;
        const std::string model = api.model;
        const std::string reasoning_effort = api.reasoning_effort;
        RuntimeConfig config = api.config;
        std::string base = model.substr(0, model.find(':'));
        json body = {{"model", base + ":online"},
                     {"usage", {{"include", true}}},  // OpenRouter: report cost
                     {"messages",
                      json::array({{{"role", "user"},
                                    {"content",
                                     "Answer concisely with source URLs. State only "
                                     "source-supported claims, preserve provider/model "
                                     "scope, and omit pricing unless asked: " +
                                         query}}})}};
        long timeout = std::max(config.web_search_timeout_s, context.timeout_s);
        long id = side_tasks.start(
            "web search", query,
            [base_url, api_key, reasoning_effort, config, body = std::move(body),
             timeout, &usage](const std::atomic<bool>& cancel) {
                auto started = std::chrono::steady_clock::now();
                debug_log("side_request",
                          {{"kind", "web_search"},
                           {"path", "/chat/completions"},
                           {"body", body}});
                Api side(config);
                side.base_url = base_url;
                side.api_key = api_key;
                side.reasoning_effort = reasoning_effort;
                json r = side.post("/chat/completions", body, timeout, &cancel);
                debug_log("side_response",
                          {{"kind", "web_search"},
                           {"duration_ms", elapsed_ms(started)},
                           {"cancelled", cancel.load()},
                           {"response", r.is_discarded() ? json(nullptr) : r}});
                if (cancel.load()) return std::string("error: web search abandoned");
                if (abort_requested()) return std::string("error: search cancelled by user");
                try {
                    if (r.is_object() && r.contains("usage")) usage.add(r["usage"]);
                    if (r.is_object() && r.contains("choices"))
                        return std::string(
                                   "[web search result; refetch only if verification is "
                                   "necessary]\n") +
                               r["choices"][0]["message"].value("content", "(empty answer)");
                    if (r.is_object() && r.contains("error"))
                        return "error: " + r["error"].value("message", "search failed");
                } catch (const std::exception&) {}
                return std::string("error: web search failed");
            },
            tool_concurrency());
        if (!id) return "error: concurrent side-task limit reached";
        long grace = std::min(context.timeout_s, timeout);
        if (auto result = side_tasks.wait(id, std::chrono::seconds(grace)))
            return result->output;
        debug_log("side_backgrounded",
                  {{"kind", "web_search"}, {"id", id}, {"query", query}});
        return "[backgrounded] web search; continue other work — the result will be "
               "delivered automatically; do not call wait_background";
        });
    t.mutating = true;
    t.summary = [](const json& a) { return a.value("query", ""); };
    t.parallel_safe = true;
    t.timeout_s = 5;
    return t;
}

// --- saved sessions ---------------------------------------------------------
// One file per conversation under ~/.uagent/history, written by Agent::save as
// two lines: a cheap header (read here for the picker) and the full payload.

struct SessionInfo {
    std::string path, cwd, title;
    long turns = 0;
    std::filesystem::file_time_type mtime;
};

// abbreviate $HOME to ~ for display
static std::string tilde(const std::string& path) {
    std::string home = user_home();
    if (!home.empty() && path.rfind(home, 0) == 0) return "~" + path.substr(home.size());
    return path;
}

static std::string api_host(const std::string& base_url) {
    return url_host(base_url);
}

#if defined(HAVE_EDITLINE)
static void configure_readline_completion(const std::vector<ModelRoute>& routes) {
    std::vector<std::string> models, efforts{"default"};
    for (const ModelRoute& route : routes) models.push_back(route.name);
    efforts.insert(efforts.end(), std::begin(REASONING_EFFORTS),
                   std::end(REASONING_EFFORTS));
    configure_completion(models, efforts);
}
#endif

static void print_model_routes(const std::vector<ModelRoute>& routes, const Api& api) {
    if (routes.empty()) {
        printf("%s* %-20s %s @ %s%s\n", BOLD(), api.model.c_str(), api.model.c_str(),
               api_host(api.base_url).c_str(), RST());
        return;
    }
    for (const ModelRoute& route : routes) {
        bool active = route.base_url == api.base_url && route.model == api.model &&
                      route.effort == api.reasoning_effort;
        printf("%s%c %-20s %s", active ? BOLD() : DIM(), active ? '*' : ' ',
               route.name.c_str(), route.model.c_str());
        if (!route.effort.empty()) printf(" · %s", route.effort.c_str());
        printf(" @ %s%s\n", api_host(route.base_url).c_str(), RST());
    }
}

static void print_available_models(Api& api, std::string filter) {
    printf("%s· querying %s/models…%s\n", DIM(), api_host(api.base_url).c_str(), RST());
    std::optional<std::vector<ModelInfo>> models = query_models(api, std::move(filter));
    if (!models) {
        printf("%s· model catalog unavailable%s\n", RED(), RST());
        return;
    }
    for (const ModelInfo& model : *models) {
        bool active = model.id == api.model;
#if defined(HAVE_EDITLINE)
        register_completion(CommandCompletion::models, model.id);
#endif
        if (active && model.context > 0) {
            api.ctx_window = model.context;
            setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
        }
        printf("%s%c %s", active ? BOLD() : DIM(), active ? '*' : ' ', model.id.c_str());
        if (model.context > 0) printf(" · ctx %s", fmt_tokens(model.context).c_str());
        if (!model.efforts.empty()) {
            printf(" · effort ");
            for (size_t i = 0; i < model.efforts.size(); ++i)
                printf("%s%s", i ? "," : "", model.efforts[i].c_str());
            if (!model.default_effort.empty())
                printf(" (default %s)", model.default_effort.c_str());
        }
        printf("%s\n", RST());
    }
    printf("%s· %zu model%s%s\n", DIM(), models->size(), models->size() == 1 ? "" : "s",
           RST());
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
static std::vector<SessionInfo> list_sessions() {
    namespace fs = std::filesystem;
    std::vector<SessionInfo> out;
    std::error_code ec;
    std::string current = canonical_cwd();
    std::string base = uagent_dir("history");
    std::string scoped = base + "/" + workspace_id(current);
    fs::create_directories(scoped, ec);
    chmod(scoped.c_str(), 0700);
    for (const std::string& dir : {scoped, base}) {
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
            std::ifstream f(e.path());
            std::string head;
            std::getline(f, head);
            json h = json::parse(head, nullptr, false);
            if (!h.is_object() || h.value("cwd", "") != current) continue;
            SessionInfo s;
            s.path = e.path().string();
            s.mtime = e.last_write_time(ec);
            s.cwd = h.value("cwd", "");
            s.turns = h.value("turns", 0L);
            s.title = h.value("title", "(untitled)");
            out.push_back(std::move(s));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const SessionInfo& a, const SessionInfo& b) { return a.mtime > b.mtime; });
    return out;
}

// print a numbered list and read a choice; returns the chosen path or "".
static std::string pick_session() {
    std::vector<SessionInfo> sessions = list_sessions();
    if (sessions.empty()) {
        printf("%s· no saved sessions%s\n", DIM(), RST());
        return "";
    }
    auto now = std::filesystem::file_time_type::clock::now();
    size_t shown = std::min(sessions.size(), size_t{20});
    for (size_t i = 0; i < shown; ++i) {
        const SessionInfo& s = sessions[i];
        long secs =
            std::chrono::duration_cast<std::chrono::seconds>(now - s.mtime).count();
        std::string safe_cwd = terminal_safe(tilde(s.cwd));
        std::string safe_title = terminal_safe(one_line(s.title, 60));
        printf("%s[%zu]%s %s · %ld turn%s · %s%s · \"%s\"%s\n", CYAN(), i + 1, RST(),
               fmt_ago(secs).c_str(), s.turns, s.turns == 1 ? "" : "s",
               DIM(), safe_cwd.c_str(), safe_title.c_str(), RST());
    }
    bool eof = false;
    std::string ans =
        trim(read_input_line("resume #: ", &eof, /*keep_history=*/false));
    if (eof || ans.empty()) return "";
    char* end = nullptr;
    long n = strtol(ans.c_str(), &end, 10);
    if (end && *end == '\0' && n >= 1 && n <= (long)shown) return sessions[n - 1].path;
    printf("%s· not a listed number%s\n", DIM(), RST());
    return "";
}

// load `path` into the agent; on success the session continues in that file
static void resume_into(Agent& agent, const std::string& path, std::string& session_file) {
    if (path.empty()) return;
    std::string error;
    if (!agent.load(path, canonical_cwd(), error)) {
        std::string safe_path = terminal_safe(path);
        std::string safe_error = terminal_safe(error);
        printf("%s· could not resume %s: %s%s\n", RED(), safe_path.c_str(),
               safe_error.c_str(), RST());
        return;
    }
    session_file = path;
    printf("%s· resumed — %zu messages%s\n", DIM(), agent.message_count() - 1, RST());
    agent.print_history();
    printf("%s· end of history, continuing%s\n", DIM(), RST());
}

// Prompt metadata stays in normal scrollback rather than a pinned TUI region.
static std::string status_bar(const Api& api, const Agent& agent, bool yolo,
                              size_t attachments, const ProcessSupervisor& processes) {
    const Usage& u = agent.session_usage();
    std::string host = api_host(api.base_url);
    long used = agent.context_used();
    std::string s = api.model + " @ " + host + " · ctx " + fmt_tokens(used);
    if (api.ctx_window > 0) {
        long pct = (used * 100 + api.ctx_window - 1) / api.ctx_window;
        s += " (" + std::to_string(pct) + "%)";
    }
    if (!api.reasoning_effort.empty()) s += " · effort " + api.reasoning_effort;
    if (u.cache_read) s += " · cache " + fmt_tokens(u.cache_read) + " total";
    if (u.cost > 0) s += " · spent " + fmt_cost(u.cost);
    if (!processes.jobs().empty()) {
        s += " · bg:";
        for (auto& j : processes.jobs()) s += " " + std::to_string(j.pid);
    }
    if (yolo) s += " · YOLO";
    if (attachments) s += " · " + std::to_string(attachments) + " attached";
    return s;
}

static void print_status_bar(const std::string& status) {
    std::string safe = terminal_safe(status);
    if (!g_tty) { printf("%s\n", safe.c_str()); return; }
    printf("%s%s\033[K%s\n", PANEL_MUTED(), safe.c_str(), RST());
}

int main(int argc, char** argv) {
    std::setlocale(LC_CTYPE, "");
    g_tty = isatty(1);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGHUP, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
#if defined(HAVE_EDITLINE)
    rl_getc_function = esc_getc;
#endif
    bool yolo = false;
    bool debug = false;
    bool trust_project = false;
    json trusted_project_config = nullptr;
    std::string debug_path;
    std::string prompt;
    bool resume_latest = false, resume_pick = false;
    g_argv0 = argv[0];
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--yolo") yolo = true;
        else if (a == "-p" && i + 1 < argc) prompt = argv[++i];
        else if (a == "--continue" || a == "-c") resume_latest = true;
        else if (a == "--resume") resume_pick = true;
        else if (a == "--trust-project-config") trust_project = true;
        else if (a == "--debug") debug = true;
        else if (a == "--version") {
            printf("uagent %s\n", UAGENT_VERSION);
            return 0;
        }
        else if (a.rfind("--debug=", 0) == 0) {
            debug = true;
            debug_path = a.substr(8);
        } else if (a == "-h" || a == "--help") {
            printf("usage: uagent [--yolo] [--trust-project-config] [--debug[=PATH]] "
                   "[-p PROMPT] [-c] [--resume]\n\n"
                   "  -p PROMPT   run one turn, print only the final answer, exit\n"
                   "  -c          resume the most recent saved session\n"
                   "  --resume    pick a saved session to resume at startup\n"
                   "  --version   print the installed version\n"
                   "  --trust-project-config  allow this workspace's .mcp.json\n\n"
                   "config: ~/.uagent/.config; process UAGENT_* variables override it\n");
            return 0;
        } else {
            fprintf(stderr, "unknown flag: %s\n", a.c_str());
            return 2;
        }
    }

    // Project MCP configuration is executable code. Trust comes only from an
    // inherited flag/environment value, a matching stored fingerprint, or an
    // explicit interactive decision. Project files cannot grant themselves.
    trust_project = trust_project ||
                    env_str("UAGENT_TRUST_PROJECT_CONFIG") == "1";
    if (!trust_project)
        trust_project = project_config_trusted(&trusted_project_config);
    if (project_config_present() && !trust_project) {
        if (!isatty(STDIN_FILENO) || !prompt.empty()) {
            fprintf(stderr,
                    "project .mcp.json is untrusted; rerun with "
                    "--trust-project-config after reviewing it\n");
            return 2;
        }
        bool eof = false;
        std::string answer = trim(read_input_line(
            "Trust and execute this workspace's .mcp.json? [y/N] ", &eof,
            /*keep_history=*/false));
        trust_project = !eof && (answer == "y" || answer == "Y" || answer == "yes");
        if (trust_project) {
            std::string error;
            if (!trust_project_config(error, &trusted_project_config)) {
                fprintf(stderr, "cannot save project trust: %s\n", error.c_str());
                return 1;
            }
        }
    }
    // Explicit trust authorizes exactly this parsed snapshot. Registration
    // consumes it directly, so a file swap after approval cannot change which
    // commands are spawned.
    if (project_config_present() && trust_project &&
        trusted_project_config.is_null()) {
        std::string error;
        if (!project_config_snapshot(trusted_project_config, error)) {
            fprintf(stderr, "cannot load trusted project config: %s\n", error.c_str());
            return 1;
        }
    }

    load_config_file();
    maintain_artifacts();
    if (!yolo) yolo = env_str("UAGENT_APPROVAL") == "yolo";
    if (!debug) {
        debug_path = env_str("UAGENT_DEBUG_LOG");
        debug = !debug_path.empty();
    }
    // Headless: everything the interactive agent prints — banner, MCP status,
    // the stream, the tool trace, the footer — goes to /dev/null, and fd 1 is
    // restored at the end to carry the answer alone. fd 2 still reports errors.
    int saved_stdout = -1;
    if (!prompt.empty()) {
        fflush(stdout);
        saved_stdout = dup(1);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1);
        close(null);
    }

    if (debug && !g_debug.start(debug_path)) {
        fprintf(stderr, "cannot open debug log: %s\n", g_debug.error().c_str());
        return 1;
    }
    if (g_debug.enabled()) {
        printf("%s· debug log: %s%s\n", DIM(), g_debug.path().c_str(), RST());
        g_debug.write("process_start",
                      {{"pid", getpid()},
                       {"cwd", std::filesystem::current_path().string()},
                       {"tty", g_tty}});
    }

    CurlRuntime curl;
    if (!curl.ready()) {
        fprintf(stderr, "cannot initialize libcurl\n");
        return 1;
    }

    AppRuntime runtime(RuntimeConfig::from_environment());
    RuntimeConfig& runtime_config = runtime.config;
    Api& api = runtime.api;
    ProviderSetup provider = configure_provider(api);
    std::vector<ModelRoute>& model_routes = provider.routes;
    if (!provider.warning.empty())
        fprintf(stderr, "%s%s%s\n", YEL(), terminal_safe(provider.warning).c_str(), RST());
#if defined(HAVE_EDITLINE)
    configure_readline_completion(model_routes);
#endif
    if (api.base_url.empty()) {
        debug_log("startup_error", {{"error", "UAGENT_BASE_URL is not set"}});
        fprintf(stderr, "%sno provider configured%s — set OPENROUTER_API_KEY or point "
                        "UAGENT_BASE_URL at an OpenAI-compatible endpoint, e.g.\n"
                        "  export UAGENT_BASE_URL=http://localhost:8080/v1\n",
                RED(), RST());
        return 1;
    }

    if (api.model.empty() || (api.ctx_window == 0 && !openrouter_url(api.base_url))) {
        auto probe_started = std::chrono::steady_clock::now();
        json models = api.get("/models");
        double probe_ms = elapsed_ms(probe_started);
        size_t offered = 0;
        if (models.is_object() && models.contains("data") && models["data"].is_array()) {
            const json& data = models["data"];
            offered = data.size();
            if (api.model.empty())
                for (const json& candidate : data)
                    if (candidate.is_object()) {
                        api.model = candidate.value("id", "");
                        if (!api.model.empty()) break;
                    }
            // ":online"-style routing suffixes are not separate /models entries
            std::string base = api.model.substr(0, api.model.find(':'));
            if (api.ctx_window == 0)
                for (const json& m : data) try {
                    std::string id = m.value("id", "");
                    if (id != api.model && id != base) continue;
                    api.ctx_window = m.value("context_length", 0L);  // OpenRouter
                    if (!api.ctx_window) api.ctx_window = m.value("max_model_len", 0L);  // vLLM
                    if (!api.ctx_window && m.contains("meta") && m["meta"].is_object())
                        api.ctx_window = m["meta"].value("n_ctx_train", 0L);  // llama.cpp
                    break;
                } catch (const std::exception&) { continue; }
        }
        // The catalog itself is never logged: on OpenRouter it is ~530 KB of
        // models we do not use, which would dwarf the rest of the trace.
        debug_log("models_probe", {{"duration_ms", probe_ms},
                                   {"models_offered", offered},
                                   {"model", api.model},
                                   {"context_window", api.ctx_window}});
        if (api.model.empty()) {
            debug_log("startup_error",
                              {{"error", "no usable model"}, {"base_url", api.base_url}});
            fprintf(stderr, "%sUAGENT_MODEL is not set and %s/models returned nothing usable%s\n",
                    RED(), api.base_url.c_str(), RST());
            return 1;
        }
    }

    auto approver = [&yolo](const Tool& t, const json& args) {
        bool granted = true;
        if (!yolo) {
            std::string q = std::string(YEL()) + "allow " + terminal_safe(t.name) + ": " +
                            terminal_safe(one_line(tool_summary(t, args), 120)) +
                            "? [Y/n] " + RST();
            bool eof = false;
            std::string ans = trim(read_input_line(q, &eof, /*keep_history=*/false));
            granted = !eof && (ans.empty() || ans == "y" || ans == "Y" || ans == "yes");
        }
        debug_log("approval",
                  {{"tool", t.name}, {"automatic", yolo}, {"granted", granted}});
        return granted;
    };

    printf("%sµAgent%s\n", BOLD(), RST());

    ProcessSupervisor& processes = runtime.processes;
    UsageAccumulator& side_usage = runtime.side_usage;
    SideTaskSupervisor& side_tasks = runtime.side_tasks;
    McpRuntime& mcp = runtime.mcp;
    bool inline_images = prompt.empty() && g_tty &&
                         terminal_image_protocol() != TerminalImageProtocol::none;
    std::vector<Tool> tools = builtin_tools(
        processes, canonical_access_path(canonical_cwd()), inline_images);
    bool has_openrouter = openrouter_url(api.base_url) ||
                          std::any_of(model_routes.begin(), model_routes.end(),
                                      [](const ModelRoute& route) {
                                          return openrouter_url(route.base_url);
                                      });
    if (has_openrouter)
        tools.push_back(web_search_tool(api, side_usage, side_tasks));  // billed side-request
    mcp_register(tools, mcp, runtime_config,
                 trusted_project_config);  // immutable trusted snapshot + user servers
    if (env_long("UAGENT_DEPTH", 0) == 0)  // only the top level delegates
        tools.push_back(subagent_tool(api, processes, yolo, debug));
    Agent agent(
        api, tools, processes, side_tasks, side_usage, approver,
        [&](std::chrono::steady_clock::time_point deadline) {
            return mcp_refresh_tools(tools, mcp, runtime_config, deadline);
        });
    if (g_debug.enabled())
        g_debug.write("session_ready",
                      {{"base_url", api.base_url},
                       {"model", api.model},
                       {"reasoning_effort", api.reasoning_effort},
                       {"configured_models", model_routes.size()},
                       {"context_window", api.ctx_window},
                       {"tools", tools.size()},
                       {"yolo", yolo},
                       {"auto_compact_pct", auto_compact_pct()},
                       {"checkpoint_mode", runtime_config.checkpoint_mode},
                       {"checkpoint_pct", checkpoint_pct()},
                       {"checkpoint_urgent_pct", checkpoint_urgent_pct()},
                       {"openrouter_provider", runtime_config.openrouter_provider},
                       {"openrouter_fallbacks", runtime_config.openrouter_fallbacks},
                       {"tool_concurrency", tool_concurrency()},
                       {"tool_result_chars", tool_result_cap()},
                       {"attachment_mb", attachment_limit_mb()},
                       {"image_detail", image_detail()},
                       {"steering", steering_enabled()},
                       {"max_tokens", max_output_tokens()},
                       {"limits", runtime_config.diagnostic_json()}});

    std::vector<Attachment> attachments;
    auto run_turns = [&](std::string input, json content = nullptr) {
        bool resume = false;
        for (;;) {
            if (resume) agent.resume();
            else agent.turn(input, std::move(content));
            if (!g_steering.take()) break;
            bool cancelled = false;
            std::string next = steering_replacement(cancelled);
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
        run_turns(prompt);
        fflush(stdout);
        dup2(saved_stdout, 1);
        close(saved_stdout);
        std::string ledger = env_str("UAGENT_USAGE_FILE");
        if (!ledger.empty()) {  // report the spend so the parent can bill it
            std::string error;
            if (!append_private_line(
                    ledger, usage_json(agent.session_usage()).dump(), error))
                fprintf(stderr, "cannot write usage ledger: %s\n", error.c_str());
        }
        std::string answer = agent.last_text();
        bg_shutdown_all(processes);
        if (answer.empty()) {  // a caller reading stdout must not see silent failure
            std::string error = agent.last_error().empty()
                                    ? "agent produced no answer"
                                    : terminal_safe(agent.last_error());
            if (g_debug.enabled())
                g_debug.write("session_end",
                              {{"reason", "headless_error"},
                               {"usage", usage_json(agent.session_usage())},
                               {"context_tokens", agent.context_used()}});
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (g_debug.enabled())
            g_debug.write("session_end",
                          {{"reason", "headless_complete"},
                           {"usage", usage_json(agent.session_usage())},
                           {"context_tokens", agent.context_used()}});
        printf("%s\n", terminal_safe(answer).c_str());
        return 0;
    }

    // current session's save file ("" until the first turn mints one)
    std::string session_file;
    if (resume_pick) resume_into(agent, pick_session(), session_file);
    else if (resume_latest) {
        std::vector<SessionInfo> s = list_sessions();
        if (s.empty()) printf("%s· no saved sessions%s\n", DIM(), RST());
        else resume_into(agent, s.front().path, session_file);
    }

    // Flush the conversation after anything that changed it — top of the loop
    // (after the previous turn) and once at exit. `saved_msgs` starts from the
    // post-resume count so a just-loaded session isn't rewritten before use.
    // Only real interactive sessions are kept; a piped `echo q | uagent` (or a
    // subagent, which pipes too) is a one-off, not something to resume.
    bool persist = isatty(0);
    uint64_t saved_revision = agent.revision();
    auto save_session = [&] {
        if (!persist || agent.message_count() <= 1 ||
            agent.revision() == saved_revision)
            return;
        if (session_file.empty())
            session_file = uagent_dir("history") + "/" + workspace_id(canonical_cwd()) + "/" +
                           utc_stamp("%Y%m%dT%H%M%SZ") + "-" +
                           std::to_string(getpid()) + ".json";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(session_file).parent_path(), ec);
        chmod(std::filesystem::path(session_file).parent_path().c_str(), 0700);
        std::string error;
        if (!agent.save(session_file, error)) {
            fprintf(stderr, "cannot save session: %s\n", error.c_str());
            return;
        }
        saved_revision = agent.revision();
    };

    std::string exit_reason = "eof";
    for (;;) {
        save_session();
        agent.drain_background();
        print_status_bar(status_bar(api, agent, yolo, attachments.size(), processes));
        panel_clear_line();
        bool eof = false;
        std::string line = read_input_line(panel_prompt(), &eof);
        if (eof) {
            if (g_tty) printf("\r\033[2K\r");
            printf("\n");
            break;
        }
        std::string input = trim(line);
        if (input.empty()) continue;

        if (input[0] == '/') debug_log("command", {{"command", input}});

        ParsedSlashCommand command = parse_slash_command(input);
        if (command.spec) {
            bool quit = false;
            switch (command.spec->id) {
                case SlashCommandId::quit:
                    exit_reason = "command";
                    quit = true;
                    break;
                case SlashCommandId::reset:
                    agent.reset();
                    attachments.clear();
                    session_file.clear();  // the old session file stays
                    saved_revision = agent.revision();
                    printf("%s· fresh session%s\n", DIM(), RST());
                    break;
                case SlashCommandId::sessions: {
                    std::string chosen = pick_session();
                    if (!chosen.empty()) {
                        resume_into(agent, chosen, session_file);
                        attachments.clear();
                        saved_revision = agent.revision();
                    }
                    break;
                }
                case SlashCommandId::models:
                    if (command.argument.empty()) {
                        print_model_routes(model_routes, api);
                        printf("%s· /models all for the live catalog; /models FILTER to search%s\n",
                               DIM(), RST());
                    } else {
                        print_available_models(api, command.argument);
                    }
                    break;
                case SlashCommandId::model: {
                    if (command.argument.empty()) {
                        print_model_routes(model_routes, api);
                        break;
                    }
                    std::string selected = select_model(api, model_routes, command.argument);
                    if (selected.empty()) {
                        printf("%s· unknown model %s; use /models%s\n", RED(),
                               terminal_safe(command.argument).c_str(), RST());
                        break;
                    }
                    agent.route_changed();
                    debug_log("route_changed",
                              {{"route", selected}, {"model", api.model},
                               {"base_url", api.base_url}, {"effort", api.reasoning_effort}});
                    printf("%s· model %s · effort %s%s\n", DIM(), selected.c_str(),
                           api.reasoning_effort.empty() ? "default"
                                                        : api.reasoning_effort.c_str(),
                           RST());
                    break;
                }
                case SlashCommandId::effort:
                    if (command.argument.empty()) {
                        printf("%s· effort %s%s\n", DIM(),
                               api.reasoning_effort.empty() ? "default"
                                                            : api.reasoning_effort.c_str(),
                               RST());
                    } else if (command.argument == "default") {
                        api.reasoning_effort.clear();
                        setenv("UAGENT_REASONING_EFFORT", "", 1);
                        agent.route_changed();
                        printf("%s· effort provider default%s\n", DIM(), RST());
                    } else if (!valid_effort(command.argument)) {
                        printf("%s· effort must be none, minimal, low, medium, high, xhigh, or "
                               "max; use default to defer to the provider%s\n", RED(), RST());
                    } else {
                        api.reasoning_effort = command.argument;
                        setenv("UAGENT_REASONING_EFFORT", command.argument.c_str(), 1);
                        agent.route_changed();
                        printf("%s· effort %s%s\n", DIM(), command.argument.c_str(), RST());
                    }
                    break;
                case SlashCommandId::yolo:
                    yolo = !yolo;
                    printf("%s· yolo %s%s\n", DIM(),
                           yolo ? "ON — auto-approving everything" : "off", RST());
                    break;
                case SlashCommandId::compact:
                    for (;;) {
                        agent.compact();
                        if (!g_steering.take()) break;
                        bool cancelled = false;
                        std::string next = steering_replacement(cancelled);
                        if (cancelled) {
                            printf("%s· resuming%s\n", DIM(), RST());
                            continue;
                        }
                        run_turns(std::move(next));
                        break;
                    }
                    break;
                case SlashCommandId::attach:
                    if (command.argument.empty()) {
                        if (attachments.empty())
                            printf("%s· no pending attachments%s\n", DIM(), RST());
                        else
                            for (const Attachment& attachment : attachments)
                                printf("%s· %s (%s)%s\n", DIM(),
                                       terminal_safe(attachment.path).c_str(),
                                       attachment.mime.c_str(), RST());
                    } else {
                        Attachment attachment;
                        std::string error;
                        if (!inspect_attachment(command.argument, attachment, error))
                            printf("%s%s%s\n", RED(), error.c_str(), RST());
                        else {
                            attachments.push_back(std::move(attachment));
                            printf("%s· attached %s for the next message%s\n", DIM(),
                                   attachments.back().name.c_str(), RST());
                        }
                    }
                    break;
                case SlashCommandId::detach:
                    attachments.clear();
                    printf("%s· attachments cleared%s\n", DIM(), RST());
                    break;
                case SlashCommandId::online: {  // OpenRouter web-search suffix
                    if (!openrouter_url(api.base_url)) {
                        printf("%s· /online is available only for OpenRouter%s\n", RED(), RST());
                        break;
                    }
                    bool on = api.model.size() > 7 &&
                              api.model.compare(api.model.size() - 7, 7, ":online") == 0;
                    if (on) api.model.erase(api.model.size() - 7);
                    else api.model += ":online";
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
            print_command_help();
            continue;
        }

        json content;
        if (!attachments.empty()) {
            std::string error;
            content = attachment_content(input, attachments, error);
            if (!error.empty()) {
                printf("%s%s%s\n", RED(), error.c_str(), RST());
                continue;
            }
            attachments.clear();
        }
        run_turns(input, std::move(content));
    }

    save_session();  // final flush: the last turn, or a /compact with no follow-up
    bg_shutdown_all(processes);
    std::remove(usage_ledger().c_str());  // a recycled pid must not inherit it

    if (g_debug.enabled()) {
        const Usage& usage = agent.session_usage();
        g_debug.write("session_end",
                      {{"reason", exit_reason},
                       {"usage", usage_json(usage)},
                       {"context_tokens", agent.context_used()}});
    }
    return 0;
}
