#pragma once
// Small shared helpers: colors, strings, private config loading, interrupt state,
// debug logging, immediate steering.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include <signal.h>
#include <unistd.h>

// --- debug-log dependencies ---------------------------------------------------
#include <chrono>
#include <cerrno>
#include <ctime>
#include <filesystem>
#include <mutex>

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "json.hpp"

// --- steering dependencies ----------------------------------------------------
#include <thread>

#include <poll.h>
#include <termios.h>

using nlohmann::json;

// --- terminal colors (empty strings when stdout is not a TTY) ---------------

inline bool g_tty = false;
inline const char* DIM() { return g_tty ? "\033[2m" : ""; }
inline const char* RST() { return g_tty ? "\033[0m" : ""; }
inline const char* CYAN() { return g_tty ? "\033[36m" : ""; }
inline const char* YEL() { return g_tty ? "\033[33m" : ""; }
inline const char* RED() { return g_tty ? "\033[31m" : ""; }
inline const char* BOLD() { return g_tty ? "\033[1m" : ""; }
inline const char* BOLD_OFF() { return g_tty ? "\033[22m" : ""; }
inline const char* ITAL() { return g_tty ? "\033[3m" : ""; }
inline const char* ITAL_OFF() { return g_tty ? "\033[23m" : ""; }
inline const char* FG_DFL() { return g_tty ? "\033[39m" : ""; }  // default foreground
inline bool light_ui() {
    static const bool light = [] {
        const char* theme = getenv("UAGENT_THEME");
        if (theme && strcmp(theme, "light") == 0) return true;
        if (theme && strcmp(theme, "dark") == 0) return false;
        const char* value = getenv("COLORFGBG");
        const char* bg = value ? strrchr(value, ';') : nullptr;
        return bg && strtol(bg + 1, nullptr, 10) >= 7;
    }();
    return light;
}
inline const char* PANEL() {
    return !g_tty ? "" : light_ui() ? "\033[38;5;234m\033[48;5;255m"
                                    : "\033[38;5;255m\033[48;5;234m";
}
inline const char* PANEL_MUTED() {
    return !g_tty ? "" : light_ui() ? "\033[38;5;243m\033[48;5;255m"
                                    : "\033[38;5;244m\033[48;5;234m";
}
inline void panel_clear_line() {
    if (!g_tty) return;
    fputs(PANEL(), stdout);
    fputs("\r\033[2K\r", stdout);
    fflush(stdout);
}
// code colors (256-color, readable on dark and light themes; glamour-inspired)
inline const char* CODE() { return g_tty ? "\033[38;5;203m" : ""; }     // inline `code`
inline const char* CODE_BLK() { return g_tty ? "\033[38;5;110m" : ""; }  // fenced block body

// --- interrupt state: Ctrl+C aborts an in-flight stream, else exits ---------

inline volatile sig_atomic_t g_streaming = 0;
inline volatile sig_atomic_t g_steering_active = 0;
// argv[0], so the agent can re-invoke itself for a subagent. A bare name is
// resolved by the child's shell via PATH; a relative one still works because
// fork keeps the cwd.
inline std::string g_argv0;

// Signals may only touch sig_atomic_t. Steering runs on an ordinary C++ thread
// and therefore uses a real atomic; abort_requested() joins the two domains.
inline volatile sig_atomic_t g_signal_abort = 0;
inline std::atomic<bool> g_thread_abort{false};
inline volatile sig_atomic_t g_child_pgid = 0;  // pgid of a running run_bash child
inline constexpr int MCP_MAX = 64;                      // tracked MCP server processes
inline volatile sig_atomic_t g_mcp_pids[MCP_MAX] = {};  // live MCP pgids — TERMed on exit
inline constexpr int BG_MAX = 64;
inline volatile sig_atomic_t g_bg_pids[BG_MAX] = {};

inline bool abort_requested() {
    return g_signal_abort != 0 || g_thread_abort.load(std::memory_order_relaxed);
}

inline void request_abort() { g_thread_abort.store(true, std::memory_order_relaxed); }

inline void clear_abort() {
    g_thread_abort.store(false, std::memory_order_relaxed);
    g_signal_abort = 0;
}

inline void sigint_handler(int signal_number) {
    if (signal_number == SIGINT && (g_streaming || g_steering_active)) {
        g_signal_abort = 1;
        return;
    }
    if (g_child_pgid > 0 && kill(-(pid_t)g_child_pgid, SIGKILL) != 0)
        kill((pid_t)g_child_pgid, SIGKILL);  // child may not have reached setsid()
    for (int i = 0; i < BG_MAX; i++)
        if (g_bg_pids[i] > 0) {
            kill(-(pid_t)g_bg_pids[i], SIGTERM);
            kill((pid_t)g_bg_pids[i], SIGTERM);
        }
    for (int i = 0; i < MCP_MAX; i++)
        if (g_mcp_pids[i] > 0) {  // whole group: servers spawn their own workers
            kill(-(pid_t)g_mcp_pids[i], SIGTERM);
            kill((pid_t)g_mcp_pids[i], SIGTERM);
        }
    _exit(128 + signal_number);
}

// Run fn with Ctrl+C wired to cancel it instead of exiting the
// program; returns true if the user cancelled. Centralizes the flag dance so
// the reset order can't drift between call sites.
template <class F>
inline bool run_cancellable(F&& fn) {
    g_streaming = 1;
    if (!abort_requested()) fn();
    g_streaming = 0;
    bool cancelled = abort_requested();
    clear_abort();
    return cancelled;
}

// --- strings ----------------------------------------------------------------

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
}

// drop one layer of matching surrounding quotes, if present
inline std::string unquote(std::string s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
}

// wrap for /bin/sh: single-quote everything, closing and re-opening around a
// literal quote ('it'\''s'), so no byte of the payload can reach the shell.
inline std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) c == '\'' ? q += "'\\''" : q += c;
    return q + "'";
}

inline bool executable_on_path(const std::string& name) {
    const char* path_value = getenv("PATH");
    if (!path_value || name.empty()) return false;
    std::string path = path_value;
    for (size_t begin = 0;;) {
        size_t end = path.find(':', begin);
        std::string directory =
            end == std::string::npos ? path.substr(begin)
                                     : path.substr(begin, end - begin);
        if (directory.empty()) directory = ".";
        if (access((directory + "/" + name).c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

// cap a string at a UTF-8 boundary (never splits a codepoint), appending "…"
inline std::string utf8_trunc(std::string s, size_t cap) {
    if (s.size() <= cap) return s;
    while (cap > 0 && ((unsigned char)s[cap] & 0xC0) == 0x80) --cap;
    s.resize(cap);
    return s += "…";
}

// Terminal column width for valid UTF-8 in the active locale. Invalid or
// incomplete sequences degrade to one column rather than breaking rendering.
inline size_t display_width(const std::string& s) {
    std::mbstate_t state{};
    size_t width = 0;
    for (size_t offset = 0; offset < s.size();) {
        wchar_t wide = 0;
        size_t consumed = std::mbrtowc(&wide, s.data() + offset, s.size() - offset, &state);
        if (consumed == static_cast<size_t>(-1) ||
            consumed == static_cast<size_t>(-2)) {
            state = {};
            ++offset;
            ++width;
            continue;
        }
        if (consumed == 0) consumed = 1;
        int columns = ::wcwidth(wide);
        if (columns > 0) width += static_cast<size_t>(columns);
        offset += consumed;
    }
    return width;
}

inline size_t json_estimated_bytes(const json& value) {
    if (value.is_string()) return value.get_ref<const std::string&>().size() + 2;
    size_t total = 16;
    if (value.is_array())
        for (const json& item : value) total += json_estimated_bytes(item);
    else if (value.is_object())
        for (const auto& [key, item] : value.items())
            total += key.size() + json_estimated_bytes(item);
    return total;
}

// first line of a string, capped — for one-line previews
inline std::string one_line(const std::string& s, size_t cap = 80) {
    return utf8_trunc(s.substr(0, s.find('\n')), cap);
}

// Model, tool and MCP text is untrusted terminal input. Preserve normal text,
// tabs and newlines but render control bytes visibly instead of letting them
// execute terminal commands. Piped output is not a terminal and remains exact.
inline std::string terminal_safe(const std::string& s) {
    if (!g_tty) return s;
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\n' || c == '\t' || c >= 0x20) {
            if (c == 0x7f) out += "\\x7f";
            else out += static_cast<char>(c);
        } else if (c == '\r') {
            out += "\\r";
        } else {
            static constexpr char hex[] = "0123456789abcdef";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

inline std::string fmt_tokens(long n) {
    char buf[32];
    if (n < 1000) snprintf(buf, sizeof buf, "%ld", n);
    else snprintf(buf, sizeof buf, "%.1fK", static_cast<double>(n) / 1000.0);
    return buf;
}

inline std::string fmt_cost(double c) {
    char buf[32];
    snprintf(buf, sizeof buf, c < 1.0 ? "$%.4f" : "$%.2f", c);
    return buf;
}

// coarse "how long ago", for the session picker
inline std::string fmt_ago(long seconds) {
    if (seconds < 60) return "just now";
    long m = seconds / 60, h = m / 60, d = h / 24;
    char buf[32];
    if (d > 0) snprintf(buf, sizeof buf, "%ldd ago", d);
    else if (h > 0) snprintf(buf, sizeof buf, "%ldh ago", h);
    else snprintf(buf, sizeof buf, "%ldm ago", m);
    return buf;
}

// --- env --------------------------------------------------------------------

inline std::string env_str(const char* name, const std::string& dflt = "") {
    const char* v = getenv(name);
    return (v && *v) ? std::string(v) : dflt;
}

inline long env_long(const char* name, long dflt) {
    const char* v = getenv(name);
    if (!v || !*v) return dflt;
    char* end = nullptr;
    long n = strtol(v, &end, 10);
    return (end && *end == '\0') ? n : dflt;
}

inline double env_double(const char* name, double dflt) {
    const char* v = getenv(name);
    if (!v || !*v) return dflt;
    char* end = nullptr;
    errno = 0;
    double n = strtod(v, &end);
    return !errno && end && *end == '\0' ? n : dflt;
}

// Single source of truth for every tunable default: the consumer and the
// session_ready debug record read the same accessor, so the trace can never
// report a value the runtime is not using.
inline long tool_result_cap() { return env_long("UAGENT_TOOL_RESULT_CHARS", 12000); }
inline long auto_compact_pct() { return env_long("UAGENT_AUTO_COMPACT_PCT", 95); }
inline long checkpoint_pct() { return env_long("UAGENT_CHECKPOINT_PCT", 65); }
inline long checkpoint_urgent_pct() {
    return env_long("UAGENT_CHECKPOINT_URGENT_PCT", 85);
}
inline long tool_concurrency() { return env_long("UAGENT_TOOL_CONCURRENCY", 4); }
inline long max_output_tokens() { return env_long("UAGENT_MAX_TOKENS", 16000); }
inline bool steering_enabled() { return env_str("UAGENT_STEERING", "1") != "0"; }

// Parsed once per process after ~/.uagent/.config is loaded. Core execution
// paths consume this typed snapshot instead of repeatedly consulting mutable
// process environment state mid-turn.
struct RuntimeConfig {
    long first_event_timeout_s = 120;
    long stream_idle_timeout_s = 90;
    long request_timeout_s = 300;
    long request_bytes = 64 * 1024 * 1024;
    long response_bytes = 32 * 1024 * 1024;
    long max_steps = 40;
    long max_tool_calls = 100;
    long max_turn_seconds = 900;
    double max_turn_cost = 1.0;
    long tool_timeout_s = 30;
    long web_search_timeout_s = 25;
    long mcp_timeout_s = 60;
    long mcp_servers = 32;
    long mcp_pages = 100;
    long mcp_tools = 256;
    long mcp_config_bytes = 1024 * 1024;
    long mcp_response_bytes = 16 * 1024 * 1024;
    long mcp_schema_bytes = 256 * 1024;
    long mcp_log_bytes = 16 * 1024 * 1024;
    long session_archive_bytes = 16 * 1024 * 1024;
    std::string checkpoint_mode = "apply";
    std::string openrouter_provider;
    bool openrouter_fallbacks = true;

    struct LongOption {
        const char* env;
        const char* name;
        long RuntimeConfig::*field;
        long minimum;
        long maximum;
    };

    inline static constexpr long ANY_MIN = std::numeric_limits<long>::min();
    inline static constexpr long ANY_MAX = std::numeric_limits<long>::max();
    inline static constexpr LongOption LONG_OPTIONS[] = {
        {"UAGENT_FIRST_EVENT_TIMEOUT", "first_event_timeout_s",
         &RuntimeConfig::first_event_timeout_s, ANY_MIN, ANY_MAX},
        {"UAGENT_STREAM_IDLE_TIMEOUT", "stream_idle_timeout_s",
         &RuntimeConfig::stream_idle_timeout_s, ANY_MIN, ANY_MAX},
        {"UAGENT_REQUEST_TIMEOUT", "request_timeout_s", &RuntimeConfig::request_timeout_s,
         ANY_MIN, ANY_MAX},
        {"UAGENT_REQUEST_BYTES", "request_bytes", &RuntimeConfig::request_bytes, 1024, ANY_MAX},
        {"UAGENT_RESPONSE_BYTES", "response_bytes", &RuntimeConfig::response_bytes, ANY_MIN,
         ANY_MAX},
        {"UAGENT_MAX_STEPS", "max_steps", &RuntimeConfig::max_steps, 1, ANY_MAX},
        {"UAGENT_MAX_TOOL_CALLS", "max_tool_calls", &RuntimeConfig::max_tool_calls, 1, ANY_MAX},
        {"UAGENT_MAX_TURN_SECONDS", "max_turn_seconds", &RuntimeConfig::max_turn_seconds, 1,
         ANY_MAX},
        {"UAGENT_TOOL_TIMEOUT", "tool_timeout_s", &RuntimeConfig::tool_timeout_s, 0, ANY_MAX},
        {"UAGENT_WEB_SEARCH_TIMEOUT", "web_search_timeout_s",
         &RuntimeConfig::web_search_timeout_s, 1, ANY_MAX},
        {"UAGENT_MCP_TIMEOUT", "mcp_timeout_s", &RuntimeConfig::mcp_timeout_s, 1, ANY_MAX},
        {"UAGENT_MCP_SERVERS", "mcp_servers", &RuntimeConfig::mcp_servers, 1, MCP_MAX},
        {"UAGENT_MCP_PAGES", "mcp_pages", &RuntimeConfig::mcp_pages, 1, ANY_MAX},
        {"UAGENT_MCP_TOOLS", "mcp_tools", &RuntimeConfig::mcp_tools, 1, ANY_MAX},
        {"UAGENT_MCP_CONFIG_BYTES", "mcp_config_bytes", &RuntimeConfig::mcp_config_bytes, 1024,
         ANY_MAX},
        {"UAGENT_MCP_RESPONSE_BYTES", "mcp_response_bytes",
         &RuntimeConfig::mcp_response_bytes, 1024, ANY_MAX},
        {"UAGENT_MCP_SCHEMA_BYTES", "mcp_schema_bytes", &RuntimeConfig::mcp_schema_bytes, 1024,
         ANY_MAX},
        {"UAGENT_MCP_LOG_BYTES", "mcp_log_bytes", &RuntimeConfig::mcp_log_bytes, 1024,
         ANY_MAX},
        {"UAGENT_SESSION_ARCHIVE_BYTES", "session_archive_bytes",
         &RuntimeConfig::session_archive_bytes, 0, ANY_MAX},
    };

    static RuntimeConfig from_environment() {
        RuntimeConfig c;
        for (const LongOption& option : LONG_OPTIONS)
            c.*option.field = std::clamp(env_long(option.env, c.*option.field),
                                         option.minimum, option.maximum);
        c.max_turn_cost = env_double("UAGENT_MAX_TURN_COST", 1.0);
        c.checkpoint_mode = env_str("UAGENT_CHECKPOINT_MODE", "apply");
        if (c.checkpoint_mode != "off" && c.checkpoint_mode != "shadow" &&
            c.checkpoint_mode != "apply")
            c.checkpoint_mode = "apply";
        c.openrouter_provider = env_str("UAGENT_OPENROUTER_PROVIDER");
        c.openrouter_fallbacks =
            env_str("UAGENT_OPENROUTER_FALLBACKS", "1") != "0";
        return c;
    }

    json diagnostic_json() const {
        json out;
        for (const LongOption& option : LONG_OPTIONS) out[option.name] = this->*option.field;
        out.update({{"max_turn_cost", max_turn_cost},
                    {"checkpoint_mode", checkpoint_mode},
                    {"checkpoint_pct", checkpoint_pct()},
                    {"checkpoint_urgent_pct", checkpoint_urgent_pct()},
                    {"auto_compact_pct", auto_compact_pct()},
                    {"openrouter_provider", openrouter_provider},
                    {"openrouter_fallbacks", openrouter_fallbacks}});
        return out;
    }
};

using EnvValues = std::map<std::string, std::string>;

// Parse without mutating the process. Only agent-owned keys are exported later;
// other entries remain available as interpolation sources without leaking every
// config value into child processes.
inline EnvValues read_env_values(const std::string& path) {
    EnvValues values;
    std::ifstream f(path);
    if (!f) return values;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;
        values[key] = unquote(val);
    }
    return values;
}

inline std::string resolve_env_value(const std::string& key, const EnvValues& values,
                                     std::set<std::string>& resolving) {
    const char* inherited = getenv(key.c_str());
    if (inherited) return inherited;
    auto found = values.find(key);
    if (found == values.end() || !resolving.insert(key).second) return "";
    const std::string& value = found->second;
    std::string out;
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '$') {
            out += value[i++];
            continue;
        }
        if (i + 1 < value.size() && value[i + 1] == '$') {
            out += '$';
            i += 2;
            continue;
        }
        size_t begin = i + 1, end = begin;
        bool braced = begin < value.size() && value[begin] == '{';
        if (braced) {
            begin++;
            end = value.find('}', begin);
            if (end == std::string::npos) { out += value[i++]; continue; }
        } else {
            while (end < value.size() &&
                   (isalnum(static_cast<unsigned char>(value[end])) || value[end] == '_'))
                ++end;
            if (end == begin) { out += value[i++]; continue; }
        }
        std::string ref = value.substr(begin, end - begin);
        out += resolve_env_value(ref, values, resolving);
        i = braced ? end + 1 : end;
    }
    resolving.erase(key);
    return out;
}

inline std::string expand_process_env(const std::string& value) {
    EnvValues none;
    none["__uagent_value"] = value;
    std::set<std::string> resolving;
    return resolve_env_value("__uagent_value", none, resolving);
}

inline std::string user_home() {
    const char* home = getenv("HOME");
    if (home && *home) return home;
    struct passwd* entry = getpwuid(getuid());
    return entry && entry->pw_dir ? entry->pw_dir : "";
}

inline std::string uagent_config_path() {
    std::string home = user_home();
    return home.empty() ? "" : home + "/.uagent/.config";
}

inline bool agent_config_key(const std::string& key) {
    return key.rfind("UAGENT_", 0) == 0 || key == "OPENROUTER_API_KEY" ||
           key == "OPENROUTER_MODEL" || key == "OPENROUTER_EFFORT";
}

// Shell exports always win. Credentials and normal settings are read only from
// ~/.uagent/.config (or an explicitly inherited UAGENT_CONFIG_FILE). Project
// .env files are application data and are never imported into the agent.
inline void load_config_file() {
    std::string custom = env_str("UAGENT_CONFIG_FILE");
    std::string path = custom.empty() ? uagent_config_path() : custom;
    if (path.empty()) return;
    EnvValues values = read_env_values(path);
    if (!values.empty()) chmod(path.c_str(), 0600);
    for (const auto& [key, ignored] : values) {
        (void)ignored;
        if (!agent_config_key(key) || getenv(key.c_str())) continue;
        std::set<std::string> resolving;
        std::string value = resolve_env_value(key, values, resolving);
        setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
    }
}

// --- debug log ---------------------------------------------------------------

inline double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

inline std::string utc_stamp(const char* format = "%Y-%m-%dT%H:%M:%SZ") {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char out[32];
    std::strftime(out, sizeof out, format, &tm);
    return out;
}

inline std::string local_stamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char out[64];
    std::strftime(out, sizeof out, "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &tm);
    return out;
}

// ~/.uagent/<sub>, created on demand. user_home() falls back to the account
// database when HOME is absent, never to a project-controlled directory.
inline std::string uagent_dir(const char* sub) {
    std::string home = user_home();
    std::string base = home.empty()
                           ? "/tmp/uagent-" + std::to_string(getuid()) + "/.uagent"
                           : home + "/.uagent";
    std::string dir = base + "/" + sub;
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    chmod(base.c_str(), 0700);
    std::filesystem::create_directories(dir, ec);
    chmod(dir.c_str(), 0700);
    return dir;
}

inline void prune_artifact_tree(const std::string& dir, long max_age_days,
                                long max_files) {
    namespace fs = std::filesystem;
    struct Entry {
        fs::path path;
        fs::file_time_type modified;
    };
    struct NewerFirst {
        bool operator()(const Entry& a, const Entry& b) const {
            return a.modified > b.modified;  // oldest entry at the top
        }
    };
    std::priority_queue<Entry, std::vector<Entry>, NewerFirst> kept;
    std::error_code ec;
    auto cutoff = fs::file_time_type::clock::now() -
                  std::chrono::hours(24 * std::max(1L, max_age_days));
    for (fs::recursive_directory_iterator it(
             dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_symlink(ec)) continue;
        if (it->is_directory(ec)) {
            chmod(it->path().c_str(), 0700);
        } else if (it->is_regular_file(ec)) {
            chmod(it->path().c_str(), 0600);
            std::error_code time_error;
            Entry entry{it->path(), it->last_write_time(time_error)};
            if (time_error) continue;
            if (entry.modified < cutoff) {
                std::error_code remove_error;
                fs::remove(entry.path, remove_error);
                continue;
            }
            if (max_files > 0) {
                kept.push(std::move(entry));
                if (kept.size() > static_cast<size_t>(max_files)) {
                    std::error_code remove_error;
                    fs::remove(kept.top().path, remove_error);
                    kept.pop();
                }
            }
        }
    }
}

inline void maintain_artifacts() {
    prune_artifact_tree(uagent_dir("history"), env_long("UAGENT_HISTORY_DAYS", 30),
                        env_long("UAGENT_HISTORY_FILES", 200));
    prune_artifact_tree(uagent_dir("sessions"), env_long("UAGENT_DEBUG_DAYS", 14),
                        env_long("UAGENT_DEBUG_FILES", 50));
    prune_artifact_tree(uagent_dir("bg"), env_long("UAGENT_BG_DAYS", 7),
                        env_long("UAGENT_BG_FILES", 200));
    prune_artifact_tree(uagent_dir("mcp"), env_long("UAGENT_MCP_LOG_DAYS", 7),
                        env_long("UAGENT_MCP_LOG_FILES", 100));
}

inline uint64_t fnv1a_update(uint64_t hash, const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string hex64(uint64_t value) {
    char out[17];
    snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(value));
    return out;
}

inline std::string safe_file_component(std::string value) {
    for (char& c : value)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            c = '_';
    if (value.empty()) value = "unnamed";
    if (value.size() > 80) value.resize(80);
    return value;
}

inline std::string canonical_cwd() {
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(std::filesystem::current_path(), ec);
    return ec ? std::filesystem::current_path().string() : path.string();
}

inline std::string url_host(std::string url) {
    if (auto p = url.find("://"); p != std::string::npos) url = url.substr(p + 3);
    if (auto p = url.find('/'); p != std::string::npos) url.resize(p);
    if (auto p = url.rfind('@'); p != std::string::npos) url = url.substr(p + 1);
    if (auto p = url.find(':'); p != std::string::npos) url.resize(p);
    std::transform(url.begin(), url.end(), url.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return url;
}

inline bool openrouter_url(std::string url) {
    url = url_host(std::move(url));
    constexpr const char* suffix = ".openrouter.ai";
    return url == "openrouter.ai" ||
           (url.size() > strlen(suffix) &&
            url.compare(url.size() - strlen(suffix), strlen(suffix), suffix) == 0);
}

inline bool openai_url(std::string url) {
    return url_host(std::move(url)) == "api.openai.com";
}

inline std::string make_session_id() {
    static std::atomic<unsigned long> sequence{0};
    std::string seed = canonical_cwd() + ":" + std::to_string(getpid()) + ":" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()) +
                       ":" + std::to_string(++sequence);
    return "uagent-" + hex64(fnv1a_update(1469598103934665603ULL,
                                          seed.data(), seed.size()));
}

inline std::string workspace_id(const std::string& root) {
    return hex64(fnv1a_update(1469598103934665603ULL, root.data(), root.size()));
}

inline bool project_config_present() {
    std::error_code ec;
    return std::filesystem::is_regular_file(".mcp.json", ec);
}

inline bool project_config_snapshot(json& snapshot, std::string& error) {
    std::error_code ec;
    uintmax_t bytes = std::filesystem::file_size(".mcp.json", ec);
    long max_bytes = std::max(1024L, env_long("UAGENT_MCP_CONFIG_BYTES", 1024 * 1024));
    if (ec || bytes > static_cast<uintmax_t>(max_bytes)) {
        error = ec ? ec.message() : "configuration exceeds byte limit";
        return false;
    }
    std::ifstream file(".mcp.json");
    snapshot = json::parse(file, nullptr, false);
    if (!snapshot.is_object()) {
        error = "project .mcp.json is not a valid JSON object";
        return false;
    }
    return true;
}

inline std::string trust_store_path() { return uagent_dir("config") + "/trusted-projects.json"; }

inline json read_trust_store() {
    std::ifstream file(trust_store_path());
    if (!file) return json::object();
    json value = json::parse(file, nullptr, false);
    return value.is_object() ? value : json::object();
}

inline bool project_config_trusted(json* trusted_snapshot = nullptr) {
    json store = read_trust_store();
    std::string root = canonical_cwd();
    json snapshot;
    std::string error;
    bool trusted = project_config_snapshot(snapshot, error) && store.contains(root) &&
                   store[root].is_object() &&
                   store[root].value("format", 0) == 2 &&
                   store[root].contains("config") &&
                   store[root]["config"] == snapshot;
    if (trusted && trusted_snapshot) *trusted_snapshot = std::move(snapshot);
    return trusted;
}

inline bool trust_project_config(std::string& error,
                                 json* trusted_snapshot = nullptr) {
    json snapshot;
    if (!project_config_snapshot(snapshot, error)) return false;
    json store = read_trust_store();
    store[canonical_cwd()] = {{"format", 2}, {"config", snapshot}};
    std::string path = trust_store_path();
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) {
        error = strerror(errno);
        return false;
    }
    fchmod(fd, 0600);
    std::string data = store.dump(2) + "\n";
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t n = write(fd, data.data() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = strerror(errno);
            close(fd);
            unlink(name.data());
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(name.data(), path.c_str()) != 0) {
        error = strerror(errno);
        unlink(name.data());
        return false;
    }
    chmod(path.c_str(), 0600);
    if (trusted_snapshot) *trusted_snapshot = std::move(snapshot);
    return true;
}

inline bool append_private_line(const std::string& path, const std::string& line,
                                std::string& error) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0) {
        error = strerror(errno);
        return false;
    }
    fchmod(fd, 0600);
    if (flock(fd, LOCK_EX) != 0) {
        error = strerror(errno);
        close(fd);
        return false;
    }
    std::string record = line + "\n";
    size_t offset = 0;
    while (offset < record.size()) {
        ssize_t written = write(fd, record.data() + offset, record.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            error = strerror(errno);
            flock(fd, LOCK_UN);
            close(fd);
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    flock(fd, LOCK_UN);
    close(fd);
    return true;
}

// Subagents are separate processes, so they append what they spent here for
// this session to fold into its own totals. Named by the parent's pid; pruned
// with the other background files.
inline std::string usage_ledger() {
    return uagent_dir("bg") + "/usage-" + std::to_string(getpid()) + ".jsonl";
}

inline std::string default_debug_path() {
    return uagent_dir("sessions") + "/" + utc_stamp("%Y%m%dT%H%M%SZ") + "-" +
           std::to_string(getpid()) + ".jsonl";
}

class DebugLog {
public:
    ~DebugLog() {
        if (file_) fclose(file_);
    }

    bool start(std::string path = "") {
        if (path.empty() || path == "1") path = default_debug_path();
        if (path.rfind("~/", 0) == 0) {
            const char* home = getenv("HOME");
            if (home && *home) path.replace(0, 1, home);
        }
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (ec) {
            error_ = ec.message();
            return false;
        }
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            error_ = strerror(errno);
            return false;
        }
        if (fchmod(fd, 0600) != 0) {
            error_ = strerror(errno);
            ::close(fd);
            return false;
        }
        file_ = fdopen(fd, "w");
        if (!file_) {
            error_ = strerror(errno);
            ::close(fd);
            return false;
        }
        std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        path_ = ec ? path : absolute.string();
        started_ = std::chrono::steady_clock::now();
        return true;
    }

    bool enabled() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }
    const std::string& error() const { return error_; }

    void write(const std::string& event, json data = json::object()) noexcept try {
        if (!file_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        json record = {{"seq", ++seq_},
                       {"time", utc_stamp()},
                       {"elapsed_ms", elapsed_ms(started_)},
                       {"event", event},
                       {"data", std::move(data)}};
        std::string line = record.dump(-1, ' ', false, json::error_handler_t::replace);
        fwrite(line.data(), 1, line.size(), file_);
        fputc('\n', file_);
        fflush(file_);
    } catch (...) {}

private:
    FILE* file_ = nullptr;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point started_;
    std::string path_, error_;
    long seq_ = 0;
};

inline DebugLog g_debug;

// compact debug logging helper — guards the enabled() check + alloc avoidance
inline void debug_log(const std::string& event, json data = json::object()) {
    if (g_debug.enabled()) g_debug.write(event, std::move(data));
}

// --- immediate steering ------------------------------------------------------
// Bare ESC cancels active work; the next prompt either submits a replacement
// message with Enter or cancels with ESC.

class Steering {
public:
    ~Steering() { stop(); }

    bool start() {
        if (!g_tty || !isatty(STDIN_FILENO) || !steering_enabled() ||
            running_.exchange(true))
            return false;
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
            running_ = false;
            return false;
        }
        termios raw = saved_;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        g_steering_active = 1;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            g_steering_active = 0;
            running_ = false;
            return false;
        }
        terminal_raw_ = true;
        try {
            thread_ = std::thread([this] { listen(); });
        } catch (...) {
            restore();
            running_ = false;
            return false;
        }
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        restore();
    }

    bool requested() const { return requested_; }

    bool take() {
        bool value = requested_.exchange(false);
        clear_abort();
        debug_log("steering_take");
        return value;
    }

private:
    void restore() {
        if (terminal_raw_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
            terminal_raw_ = false;
        }
        g_steering_active = 0;
        if (g_tty) {
            fputs(RST(), stdout);
            fflush(stdout);
        }
    }

    void listen() {
        while (running_) {
            struct pollfd input = {STDIN_FILENO, POLLIN, 0};
            if (poll(&input, 1, 100) <= 0 || !(input.revents & POLLIN)) continue;
            unsigned char bytes[64];
            ssize_t count = read(STDIN_FILENO, bytes, sizeof bytes);
            for (ssize_t i = 0; i < count && running_; ++i) {
                if (bytes[i] != 0x1b) continue;
                if (i + 1 < count) break;  // arrow/Alt escape sequence
                struct pollfd next = {STDIN_FILENO, POLLIN, 0};
                if (poll(&next, 1, 30) > 0) {
                    unsigned char discard[16];
                    ssize_t discarded = read(STDIN_FILENO, discard, sizeof discard);
                    (void)discarded;
                    break;
                }
                requested_ = true;
                request_abort();
                debug_log("steering_interrupt");
                return;
            }
        }
    }

    std::atomic<bool> running_{false}, requested_{false};
    bool terminal_raw_ = false;
    termios saved_{};
    std::thread thread_;
};

inline Steering g_steering;

class SteeringGuard {
public:
    explicit SteeringGuard(bool enabled = true) : active_(enabled && g_steering.start()) {}
    ~SteeringGuard() { stop(); }
    void stop() {
        if (!active_) return;
        g_steering.stop();
        active_ = false;
    }

private:
    bool active_;
};
