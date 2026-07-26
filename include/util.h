// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UTIL_H_
#define UAGENT_INCLUDE_UTIL_H_
// Small shared helpers: colors, strings, private config loading, interrupt
// state, debug logging, immediate steering.

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/json.hpp"

namespace uagent {

using nlohmann::json;

inline bool JsonBool(const json& object, const char* key,
                     bool fallback = false) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_boolean() ? value->get<bool>()
                                                      : fallback;
}

inline int64_t JsonInt(const json& object, const char* key,
                       int64_t fallback = 0) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  if (value == object.end()) return fallback;
  if (value->is_number_unsigned()) {
    uint64_t number = value->get<uint64_t>();
    return number <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? static_cast<int64_t>(number)
               : fallback;
  }
  return value->is_number_integer() ? value->get<int64_t>() : fallback;
}

inline double JsonNumber(const json& object, const char* key,
                         double fallback = 0.0) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_number() ? value->get<double>()
                                                     : fallback;
}

inline std::string JsonString(const json& object, const char* key,
                              std::string fallback = {}) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_string() ? value->get<std::string>()
                                                     : fallback;
}

inline bool JsonValue(const json& object, const char* key, bool fallback) {
  return JsonBool(object, key, fallback);
}

template <std::integral Integer>
  requires(!std::same_as<Integer, bool>)
inline Integer JsonValue(const json& object, const char* key,
                         Integer fallback) {
  int64_t value = JsonInt(object, key, static_cast<int64_t>(fallback));
  if constexpr (std::signed_integral<Integer>) {
    if (value < static_cast<int64_t>(std::numeric_limits<Integer>::lowest()) ||
        value > static_cast<int64_t>(std::numeric_limits<Integer>::max())) {
      return fallback;
    }
  } else if (value < 0 ||
             static_cast<uint64_t>(value) >
                 static_cast<uint64_t>(std::numeric_limits<Integer>::max())) {
    return fallback;
  }
  return static_cast<Integer>(value);
}

template <std::floating_point Number>
inline Number JsonValue(const json& object, const char* key, Number fallback) {
  return static_cast<Number>(JsonNumber(object, key, fallback));
}

inline std::string JsonValue(const json& object, const char* key,
                             const char* fallback) {
  return JsonString(object, key, fallback);
}

inline std::string JsonValue(const json& object, const char* key,
                             const std::string& fallback) {
  return JsonString(object, key, fallback);
}

inline json JsonValue(const json& object, const char* key, json fallback) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value == object.end() ? fallback : *value;
}

#ifdef UAGENT_VERSION
inline constexpr char kVersion[] = UAGENT_VERSION;
#else
inline constexpr char kVersion[] = "dev";
#endif

// External bytes (tool output, filenames, pasted input) may be malformed UTF-8,
// which plain dump() throws on; substitute U+FFFD instead.
inline std::string JsonDump(const json& value, int indent = -1) {
  return value.dump(indent, ' ', false, json::error_handler_t::replace);
}

// --- terminal colors (empty strings when stdout is not a TTY) ---------------

inline bool g_tty = false;
inline volatile sig_atomic_t g_signal_tty = 0;
inline constexpr char kTerminalRestore[] = "\033[0m\033[39m\033[49m";
// Separate from TERMINAL_RESTORE, which RST() emits mid-stream as a pure SGR
// reset.
inline constexpr char kTerminalModeReset[] = "\033[?2004l";
inline const char* DIM() { return g_tty ? "\033[2m" : ""; }
inline const char* RST() { return g_tty ? kTerminalRestore : ""; }
inline const char* CYAN() { return g_tty ? "\033[36m" : ""; }
inline const char* YEL() { return g_tty ? "\033[33m" : ""; }
inline const char* RED() { return g_tty ? "\033[31m" : ""; }
inline const char* BOLD() { return g_tty ? "\033[1m" : ""; }
inline const char* BoldOff() { return g_tty ? "\033[22m" : ""; }
inline const char* ITAL() { return g_tty ? "\033[3m" : ""; }
inline const char* ItalOff() { return g_tty ? "\033[23m" : ""; }
inline const char* FgDfl() {
  return g_tty ? "\033[39m" : "";
}  // default foreground
inline bool LightUi() {
  static const bool kLight = [] {
    const char* theme = getenv("UAGENT_THEME");
    if (theme && strcmp(theme, "light") == 0) return true;
    if (theme && strcmp(theme, "dark") == 0) return false;
    const char* value = getenv("COLORFGBG");
    const char* bg = value ? strrchr(value, ';') : nullptr;
    return bg && strtol(bg + 1, nullptr, 10) >= 7;
  }();
  return kLight;
}
inline const char* PANEL() {
  return !g_tty      ? ""
         : LightUi() ? "\033[38;5;234m\033[48;5;255m"
                     : "\033[38;5;255m\033[48;5;234m";
}
inline const char* PanelMuted() {
  return !g_tty      ? ""
         : LightUi() ? "\033[38;5;243m\033[48;5;255m"
                     : "\033[38;5;244m\033[48;5;234m";
}
inline void PanelClearLine() {
  if (!g_tty) return;
  fputs(PANEL(), stdout);
  fputs("\r\033[2K\r", stdout);
  fflush(stdout);
}
inline void TerminalRestore() {
  if (!g_tty) return;
  fputs(kTerminalRestore, stdout);
  fputs(kTerminalModeReset, stdout);
  fflush(stdout);
}
inline void TerminalClearToEnd() {
  if (!g_tty) return;
  fputs("\r\033[K", stdout);
  fflush(stdout);
}
// Wraps pasted text in \e[200~ … \e[201~ so a multi-line paste arrives as one
// unit.
inline void BracketedPaste(bool on) {
  if (!g_tty) return;
  fputs(on ? "\033[?2004h" : kTerminalModeReset, stdout);
  fflush(stdout);
}

// Animates while a call blocks with nothing to print. stop() is idempotent and
// wakes the thread immediately — it runs on the first-streamed-byte path.
class TerminalSpinner {
 public:
  explicit TerminalSpinner(bool enabled = true) {
    if (!enabled || !g_tty) return;
    thread_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      for (int i = 0; !done_; i = (i + 1) & 3) {
        printf("\r%s%c%s", DIM(), "|/-\\"[i], RST());
        fflush(stdout);
        wake_.wait_for(lock, std::chrono::milliseconds(100),
                       [this] { return done_; });
      }
    });
  }

  ~TerminalSpinner() { Stop(); }
  TerminalSpinner(const TerminalSpinner&) = delete;
  TerminalSpinner& operator=(const TerminalSpinner&) = delete;

  void Stop() {
    if (!thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    wake_.notify_one();
    thread_.join();
    TerminalClearToEnd();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool done_ = false;
  std::thread thread_;
};

// code colors (256-color, readable on dark and light themes; glamour-inspired)
inline const char* CODE() {
  return g_tty ? "\033[38;5;203m" : "";
}  // inline `code`
inline const char* CodeBlk() {
  return g_tty ? "\033[38;5;110m" : "";
}  // fenced block body
inline const char* MATH() { return g_tty ? "\033[38;5;141m" : ""; }  // LaTeX

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
inline constexpr int kFgMax = 16;  // concurrent foreground shells
inline volatile sig_atomic_t g_child_pgids[kFgMax] = {};
inline constexpr int kMcpMax = 64;  // tracked MCP server processes
inline volatile sig_atomic_t g_mcp_pids[kMcpMax] =
    {};  // live MCP pgids — TERMed on exit
inline constexpr int kBgMax = 64;
inline volatile sig_atomic_t g_bg_pids[kBgMax] = {};

// Pid slot tables read by the signal handler. Parallel tool workers claim slots
// concurrently, so writers serialise here; the handler only ever reads.
inline void TrackPid(volatile sig_atomic_t* slots, int count, pid_t pid,
                     bool add) {
  static std::mutex slots_mutex;
  std::lock_guard<std::mutex> lock(slots_mutex);
  for (int i = 0; i < count; ++i) {
    if (add ? slots[i] == 0 : slots[i] == pid) {
      slots[i] = add ? pid : 0;
      return;
    }
  }
}

inline bool AbortRequested() {
  return g_signal_abort != 0 || g_thread_abort.load(std::memory_order_relaxed);
}

inline void RequestAbort() {
  g_thread_abort.store(true, std::memory_order_relaxed);
}

inline void ClearAbort() {
  g_thread_abort.store(false, std::memory_order_relaxed);
  g_signal_abort = 0;
}

inline void SigintHandler(int signal_number) {
  if (signal_number == SIGINT && (g_streaming || g_steering_active)) {
    g_signal_abort = 1;
    return;
  }
  for (int i = 0; i < kFgMax; i++) {
    if (g_child_pgids[i] > 0 &&
        kill(-static_cast<pid_t>(g_child_pgids[i]), SIGKILL) != 0) {
      kill(static_cast<pid_t>(g_child_pgids[i]),
           SIGKILL);  // may not have reached setsid()
    }
  }
  for (int i = 0; i < kBgMax; i++) {
    if (g_bg_pids[i] > 0) {
      kill(-static_cast<pid_t>(g_bg_pids[i]), SIGTERM);
      kill(static_cast<pid_t>(g_bg_pids[i]), SIGTERM);
    }
  }
  for (int i = 0; i < kMcpMax; i++) {
    if (g_mcp_pids[i] > 0) {  // whole group: servers spawn their own workers
      kill(-static_cast<pid_t>(g_mcp_pids[i]), SIGTERM);
      kill(static_cast<pid_t>(g_mcp_pids[i]), SIGTERM);
    }
  }
  if (g_signal_tty) {
    ssize_t restored =
        write(STDOUT_FILENO, kTerminalRestore, sizeof(kTerminalRestore) - 1);
    restored = write(STDOUT_FILENO, kTerminalModeReset,
                     sizeof(kTerminalModeReset) - 1);
    (void)restored;
  }
  _exit(128 + signal_number);
}

// Run fn with Ctrl+C wired to cancel it instead of exiting the
// program; returns true if the user cancelled. Centralizes the flag dance so
// the reset order can't drift between call sites.
template <class F>
inline bool RunCancellable(F&& fn) {
  g_streaming = 1;
  if (!AbortRequested()) fn();
  g_streaming = 0;
  bool cancelled = AbortRequested();
  ClearAbort();
  return cancelled;
}

// --- strings ----------------------------------------------------------------

inline std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

inline void ReplaceAll(std::string& s, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return;
  for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;
       pos += to.size()) {
    s.replace(pos, from.size(), to);
  }
}

// drop one layer of matching surrounding quotes, if present
inline std::string Unquote(std::string s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  return s;
}

// wrap for /bin/sh: single-quote everything, closing and re-opening around a
// literal quote ('it'\''s'), so no byte of the payload can reach the shell.
inline std::string ShellQuote(const std::string& s) {
  std::string q = "'";
  for (char c : s) c == '\'' ? q += "'\\''" : q += c;
  return q + "'";
}

inline bool ExecutableOnPath(const std::string& name) {
  const char* path_value = getenv("PATH");
  if (!path_value || name.empty()) return false;
  std::string path = path_value;
  for (size_t begin = 0;;) {
    size_t end = path.find(':', begin);
    std::string directory = end == std::string::npos
                                ? path.substr(begin)
                                : path.substr(begin, end - begin);
    if (directory.empty()) directory = ".";
    if (access((directory + "/" + name).c_str(), X_OK) == 0) return true;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return false;
}

inline size_t Utf8BoundaryBefore(const std::string& s, size_t offset) {
  offset = std::min(offset, s.size());
  while (offset > 0 && offset < s.size() &&
         (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) {
    --offset;
  }
  return offset;
}

inline size_t Utf8BoundaryAfter(const std::string& s, size_t offset) {
  offset = std::min(offset, s.size());
  while (offset < s.size() &&
         (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) {
    ++offset;
  }
  return offset;
}

// cap a string at a UTF-8 boundary (never splits a codepoint), appending "…"
inline std::string Utf8Prefix(std::string s, size_t cap) {
  if (s.size() <= cap) return s;
  s.resize(Utf8BoundaryBefore(s, cap));
  return s;
}

inline std::string Utf8Trunc(std::string s, size_t cap) {
  if (s.size() <= cap) return s;
  return Utf8Prefix(std::move(s), cap) + "…";
}

// Terminal column width for valid UTF-8 in the active locale. Invalid or
// incomplete sequences degrade to one column rather than breaking rendering.
inline size_t DisplayWidth(const std::string& s) {
  std::mbstate_t state{};
  size_t width = 0;
  for (size_t offset = 0; offset < s.size();) {
    wchar_t wide = 0;
    size_t consumed =
        std::mbrtowc(&wide, s.data() + offset, s.size() - offset, &state);
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

inline size_t JsonEstimatedBytes(const json& value) {
  if (value.is_string()) return value.get_ref<const std::string&>().size() + 2;
  size_t total = 16;
  if (value.is_array()) {
    for (const json& item : value) total += JsonEstimatedBytes(item);
  } else if (value.is_object()) {
    for (const auto& [key, item] : value.items()) {
      total += key.size() + JsonEstimatedBytes(item);
    }
  }
  return total;
}

// first line of a string, capped — for one-line previews
inline std::string StripTrailingSlashes(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

inline std::string OneLine(const std::string& s, size_t cap = 80) {
  return Utf8Trunc(s.substr(0, s.find('\n')), cap);
}

// Model, tool and MCP text is untrusted terminal input. Preserve normal text,
// tabs and newlines but render control bytes visibly instead of letting them
// execute terminal commands. Piped output is not a terminal and remains exact.
inline std::string TerminalSafe(const std::string& s) {
  if (!g_tty) return s;
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c == '\n' || c == '\t' || c >= 0x20) {
      if (c == 0x7f) {
        out += "\\x7f";
      } else {
        out += static_cast<char>(c);
      }
    } else if (c == '\r') {
      out += "\\r";
    } else {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\x";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

inline std::string FmtTokens(int64_t n) {
  if (n < 1000) return std::to_string(n);
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << static_cast<double>(n) / 1000.0
      << 'K';
  return out.str();
}

inline std::string FmtCost(double c) {
  std::ostringstream out;
  out << '$' << std::fixed << std::setprecision(c < 1.0 ? 4 : 2) << c;
  return out.str();
}

// coarse "how long ago", for the session picker
inline std::string FmtAgo(int64_t seconds) {
  if (seconds < 60) return "just now";
  int64_t m = seconds / 60, h = m / 60, d = h / 24;
  if (d > 0) return std::to_string(d) + "d ago";
  if (h > 0) return std::to_string(h) + "h ago";
  return std::to_string(m) + "m ago";
}

// --- env --------------------------------------------------------------------

inline std::string EnvStr(const char* name, const std::string& dflt = "") {
  const char* v = getenv(name);
  return (v && *v) ? std::string(v) : dflt;
}

inline int64_t EnvLong(const char* name, int64_t dflt) {
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  int64_t n = strtol(v, &end, 10);
  return (end && *end == '\0') ? n : dflt;
}

inline int64_t TerminalColumns() {
  struct winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    return size.ws_col;
  }
  return std::max(int64_t{1}, EnvLong("COLUMNS", 80));
}

// Fit text into the rest of the row. Callers pass the decoration itself, not a
// hand-counted column budget, so the two cannot drift.
inline std::string TerminalFit(const std::string& text,
                               const std::string& prefix = "",
                               const std::string& suffix = "") {
  int64_t reserve =
      static_cast<int64_t>(DisplayWidth(prefix) + DisplayWidth(suffix));
  return OneLine(text, static_cast<size_t>(std::max(
                           int64_t{1}, TerminalColumns() - reserve - 2)));
}

inline double EnvDouble(const char* name, double dflt) {
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  errno = 0;
  double n = strtod(v, &end);
  return !errno && end && *end == '\0' ? n : dflt;
}

// Accessors shared by their consumers and the session_ready diagnostic.
inline int64_t ToolResultCap() {
  return EnvLong("UAGENT_TOOL_RESULT_CHARS", 8000);
}
inline int64_t AutoCompactPct() {
  return EnvLong("UAGENT_AUTO_COMPACT_PCT", 95);
}
inline int64_t CheckpointPct() { return EnvLong("UAGENT_CHECKPOINT_PCT", 65); }
inline int64_t CheckpointUrgentPct() {
  return EnvLong("UAGENT_CHECKPOINT_URGENT_PCT", 85);
}
inline int64_t ToolConcurrency() {
  return std::clamp(EnvLong("UAGENT_TOOL_CONCURRENCY", 4), int64_t{1},
                    static_cast<int64_t>(kFgMax));
}
// Delegation depth: 0 is the interactive coordinator. A subagent may delegate
// again while it stays under the cap, so nesting is bounded, not banned.
inline int64_t AgentDepth() {
  return std::max(int64_t{0}, EnvLong("UAGENT_DEPTH", 0));
}
inline bool CanDelegate() {
  return AgentDepth() <
         std::max(int64_t{0}, EnvLong("UAGENT_SUBAGENT_DEPTH", 2));
}
// Budgets handed to a delegated child. Lower than the coordinator's own, so one
// flailing subagent cannot spend the whole turn.
inline int64_t SubagentMaxSteps() {
  return EnvLong("UAGENT_SUBAGENT_MAX_STEPS", 25);
}
inline int64_t SubagentMaxToolCalls() {
  return EnvLong("UAGENT_SUBAGENT_MAX_TOOL_CALLS", 60);
}
inline int64_t MaxOutputTokens() { return EnvLong("UAGENT_MAX_TOKENS", 16000); }
inline bool SteeringEnabled() { return EnvStr("UAGENT_STEERING", "1") != "0"; }

// Core request, MCP, and persistence settings, parsed once after
// ~/.uagent/.config is loaded.
struct RuntimeConfig {
  int64_t first_event_timeout_s = 120;
  int64_t stream_idle_timeout_s = 90;
  int64_t request_timeout_s = 300;
  int64_t request_bytes = 64 * 1024 * 1024;
  int64_t response_bytes = 32 * 1024 * 1024;
  int64_t max_steps = 40;
  int64_t max_tool_calls = 100;
  int64_t max_turn_seconds = 900;
  double max_turn_cost = 1.0;
  int64_t tool_timeout_s = 30;
  int64_t web_search_timeout_s = 25;
  int64_t web_search_max_tokens = 1200;
  int64_t web_search_calls = 2;
  int64_t mcp_timeout_s = 60;
  int64_t mcp_servers = 32;
  int64_t mcp_pages = 100;
  int64_t mcp_tools = 256;
  int64_t mcp_config_bytes = 1024 * 1024;
  int64_t mcp_response_bytes = 16 * 1024 * 1024;
  int64_t mcp_schema_bytes = 256 * 1024;
  int64_t mcp_log_bytes = 16 * 1024 * 1024;
  int64_t project_doc_bytes = 32 * 1024;
  int64_t session_archive_bytes = 16 * 1024 * 1024;
  std::string checkpoint_mode = "apply";
  std::string openrouter_provider;
  std::string web_search_effort;
  std::string web_search_model;
  bool openrouter_fallbacks = true;

  struct LongOption {
    const char* env;
    const char* name;
    int64_t RuntimeConfig::* field;
    int64_t minimum;
    int64_t maximum;
  };
  struct StringOption {
    const char* env;
    const char* name;
    std::string RuntimeConfig::* field;
    const char* default_value;
  };
  struct BoolOption {
    const char* env;
    const char* name;
    bool RuntimeConfig::* field;
    bool default_value;
  };

  inline static constexpr int64_t kAnyMin = std::numeric_limits<int64_t>::min();
  inline static constexpr int64_t kAnyMax = std::numeric_limits<int64_t>::max();
  inline static constexpr LongOption kLongOptions[] = {
      {"UAGENT_FIRST_EVENT_TIMEOUT", "first_event_timeout_s",
       &RuntimeConfig::first_event_timeout_s, kAnyMin, kAnyMax},
      {"UAGENT_STREAM_IDLE_TIMEOUT", "stream_idle_timeout_s",
       &RuntimeConfig::stream_idle_timeout_s, kAnyMin, kAnyMax},
      {"UAGENT_REQUEST_TIMEOUT", "request_timeout_s",
       &RuntimeConfig::request_timeout_s, kAnyMin, kAnyMax},
      {"UAGENT_REQUEST_BYTES", "request_bytes", &RuntimeConfig::request_bytes,
       1024, kAnyMax},
      {"UAGENT_RESPONSE_BYTES", "response_bytes",
       &RuntimeConfig::response_bytes, kAnyMin, kAnyMax},
      {"UAGENT_MAX_STEPS", "max_steps", &RuntimeConfig::max_steps, 1, kAnyMax},
      {"UAGENT_MAX_TOOL_CALLS", "max_tool_calls",
       &RuntimeConfig::max_tool_calls, 1, kAnyMax},
      {"UAGENT_MAX_TURN_SECONDS", "max_turn_seconds",
       &RuntimeConfig::max_turn_seconds, 1, kAnyMax},
      {"UAGENT_TOOL_TIMEOUT", "tool_timeout_s", &RuntimeConfig::tool_timeout_s,
       0, kAnyMax},
      {"UAGENT_WEB_SEARCH_TIMEOUT", "web_search_timeout_s",
       &RuntimeConfig::web_search_timeout_s, 1, kAnyMax},
      {"UAGENT_WEB_SEARCH_MAX_TOKENS", "web_search_max_tokens",
       &RuntimeConfig::web_search_max_tokens, 128, kAnyMax},
      {"UAGENT_WEB_SEARCH_CALLS", "web_search_calls",
       &RuntimeConfig::web_search_calls, 1, kAnyMax},
      {"UAGENT_MCP_TIMEOUT", "mcp_timeout_s", &RuntimeConfig::mcp_timeout_s, 1,
       kAnyMax},
      {"UAGENT_MCP_SERVERS", "mcp_servers", &RuntimeConfig::mcp_servers, 1,
       kMcpMax},
      {"UAGENT_MCP_PAGES", "mcp_pages", &RuntimeConfig::mcp_pages, 1, kAnyMax},
      {"UAGENT_MCP_TOOLS", "mcp_tools", &RuntimeConfig::mcp_tools, 1, kAnyMax},
      {"UAGENT_MCP_CONFIG_BYTES", "mcp_config_bytes",
       &RuntimeConfig::mcp_config_bytes, 1024, kAnyMax},
      {"UAGENT_MCP_RESPONSE_BYTES", "mcp_response_bytes",
       &RuntimeConfig::mcp_response_bytes, 1024, kAnyMax},
      {"UAGENT_MCP_SCHEMA_BYTES", "mcp_schema_bytes",
       &RuntimeConfig::mcp_schema_bytes, 1024, kAnyMax},
      {"UAGENT_MCP_LOG_BYTES", "mcp_log_bytes", &RuntimeConfig::mcp_log_bytes,
       1024, kAnyMax},
      {"UAGENT_PROJECT_DOC_BYTES", "project_doc_bytes",
       &RuntimeConfig::project_doc_bytes, 0, kAnyMax},
      {"UAGENT_SESSION_ARCHIVE_BYTES", "session_archive_bytes",
       &RuntimeConfig::session_archive_bytes, 0, kAnyMax},
  };
  inline static constexpr StringOption kStringOptions[] = {
      {"UAGENT_CHECKPOINT_MODE", "checkpoint_mode",
       &RuntimeConfig::checkpoint_mode, "apply"},
      {"UAGENT_OPENROUTER_PROVIDER", "openrouter_provider",
       &RuntimeConfig::openrouter_provider, ""},
      {"UAGENT_WEB_SEARCH_EFFORT", "web_search_effort",
       &RuntimeConfig::web_search_effort, ""},
      {"UAGENT_WEB_SEARCH_MODEL", "web_search_model",
       &RuntimeConfig::web_search_model, ""},
  };
  inline static constexpr BoolOption kBoolOptions[] = {
      {"UAGENT_OPENROUTER_FALLBACKS", "openrouter_fallbacks",
       &RuntimeConfig::openrouter_fallbacks, true},
  };

  static RuntimeConfig FromEnvironment() {
    RuntimeConfig c;
    for (const LongOption& option : kLongOptions) {
      c.*option.field = std::clamp(EnvLong(option.env, c.*option.field),
                                   option.minimum, option.maximum);
    }
    for (const StringOption& option : kStringOptions) {
      c.*option.field = EnvStr(option.env, option.default_value);
    }
    for (const BoolOption& option : kBoolOptions) {
      c.*option.field =
          EnvStr(option.env, option.default_value ? "1" : "0") != "0";
    }
    c.max_turn_cost = EnvDouble("UAGENT_MAX_TURN_COST", 1.0);
    if (c.checkpoint_mode != "off" && c.checkpoint_mode != "shadow" &&
        c.checkpoint_mode != "apply") {
      c.checkpoint_mode = "apply";
    }
    return c;
  }

  json DiagnosticJson() const {
    json out;
    for (const LongOption& option : kLongOptions) {
      out[option.name] = this->*option.field;
    }
    for (const StringOption& option : kStringOptions) {
      out[option.name] = this->*option.field;
    }
    for (const BoolOption& option : kBoolOptions) {
      out[option.name] = this->*option.field;
    }
    out.update({
        {"max_turn_cost", max_turn_cost},
        {"checkpoint_pct", CheckpointPct()},
        {"checkpoint_urgent_pct", CheckpointUrgentPct()},
        {"auto_compact_pct", AutoCompactPct()},
    });
    return out;
  }
};

using EnvValues = std::map<std::string, std::string>;

// Parse without mutating the process. Only agent-owned keys are exported later;
// other entries remain available as interpolation sources without leaking every
// config value into child processes.
inline EnvValues ReadEnvValues(const std::string& path) {
  EnvValues values;
  std::ifstream f(path);
  if (!f) return values;
  std::string line;
  while (std::getline(f, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.starts_with("export ")) line = Trim(line.substr(7));
    size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string key = Trim(line.substr(0, eq));
    std::string val = Trim(line.substr(eq + 1));
    if (key.empty()) continue;
    values[key] = Unquote(val);
  }
  return values;
}

inline std::string ResolveEnvValue(const std::string& key,
                                   const EnvValues& values,
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
      if (end == std::string::npos) {
        out += value[i++];
        continue;
      }
    } else {
      while (end < value.size() &&
             (isalnum(static_cast<unsigned char>(value[end])) ||
              value[end] == '_')) {
        ++end;
      }
      if (end == begin) {
        out += value[i++];
        continue;
      }
    }
    std::string ref = value.substr(begin, end - begin);
    out += ResolveEnvValue(ref, values, resolving);
    i = braced ? end + 1 : end;
  }
  resolving.erase(key);
  return out;
}

inline std::string ExpandProcessEnv(const std::string& value) {
  EnvValues none;
  none["__uagent_value"] = value;
  std::set<std::string> resolving;
  return ResolveEnvValue("__uagent_value", none, resolving);
}

inline std::string UserHome() {
  const char* home = getenv("HOME");
  if (home && *home) return home;
  int64_t buffer_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (buffer_size < 0) buffer_size = 16 * 1024;
  std::vector<char> buffer(static_cast<size_t>(buffer_size));
  struct passwd entry{};
  struct passwd* result = nullptr;
  if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) !=
      0) {
    return "";
  }
  return result && entry.pw_dir ? entry.pw_dir : "";
}

inline std::string UagentConfigPath() {
  std::string home = UserHome();
  return home.empty() ? "" : home + "/.uagent/.config";
}

// Atomic shared writer for config, trust state, tools, and preferences. A temp
// file in the target directory makes replacement crash-safe.
inline bool AtomicWriteFile(const std::string& path, const std::string& content,
                            mode_t create_mode, bool preserve_mode,
                            std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path target(path);
  if (target.has_parent_path()) {
    fs::create_directories(target.parent_path(), ec);
  }
  if (ec) {
    error = "cannot create parent directory for " + path + ": " + ec.message();
    return false;
  }
  if (fs::is_symlink(target, ec)) {
    target = fs::canonical(target, ec);
    if (ec) {
      error = "cannot resolve symlink " + path + ": " + ec.message();
      return false;
    }
  }
  fs::path parent =
      target.has_parent_path() ? target.parent_path() : fs::path(".");
  std::string pattern =
      (parent / ("." + target.filename().string() + ".uagent.XXXXXX")).string();
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back('\0');
  int fd = mkstemp(temp.data());
  if (fd < 0) {
    error = "cannot create temporary file for " + path + ": " + strerror(errno);
    return false;
  }
  // `message` is built by the caller before unlink() can clobber errno.
  auto fail = [&](std::string message) {
    error = std::move(message);
    unlink(temp.data());
    return false;
  };
  struct stat st{};
  mode_t mode = preserve_mode && stat(target.c_str(), &st) == 0
                    ? st.st_mode & 07777
                    : create_mode;
  if (fchmod(fd, mode) != 0) {
    std::string message = strerror(errno);
    close(fd);
    return fail(message);
  }
  for (size_t offset = 0; offset < content.size();) {
    ssize_t written =
        write(fd, content.data() + offset, content.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      std::string message = "write to " + path + " failed: " + strerror(errno);
      close(fd);
      return fail(message);
    }
    offset += static_cast<size_t>(written);
  }
  int failure = fsync(fd) != 0 ? errno : 0;
  if (close(fd) != 0 && !failure) failure = errno;
  if (failure) {
    return fail("write to " + path + " failed: " + strerror(failure));
  }
  if (rename(temp.data(), target.c_str()) != 0) {
    return fail("cannot replace " + path + ": " + strerror(errno));
  }
  return true;
}

inline bool AgentConfigKey(const std::string& key) {
  return key.starts_with("UAGENT_") || key == "OPENROUTER_API_KEY" ||
         key == "OPENROUTER_MODEL" || key == "OPENROUTER_EFFORT";
}

// Shell exports always win. Credentials and normal settings are read only from
// ~/.uagent/.config (or an explicitly inherited UAGENT_CONFIG_FILE). Project
// .env files are application data and are never imported into the agent.
inline void LoadConfigFile() {
  std::string custom = EnvStr("UAGENT_CONFIG_FILE");
  std::string path = custom.empty() ? UagentConfigPath() : custom;
  if (path.empty()) return;
  EnvValues values = ReadEnvValues(path);
  if (!values.empty()) chmod(path.c_str(), 0600);
  for (const auto& [key, ignored] : values) {
    (void)ignored;
    if (!AgentConfigKey(key) || getenv(key.c_str())) continue;
    std::set<std::string> resolving;
    std::string value = ResolveEnvValue(key, values, resolving);
    setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
  }
}

// --- debug log ---------------------------------------------------------------

inline double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

inline std::string UtcStamp(const char* format = "%Y-%m-%dT%H:%M:%SZ") {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  char out[32];
  std::strftime(out, sizeof out, format, &tm);
  return out;
}

inline std::string LocalStamp() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char out[64];
  std::strftime(out, sizeof out, "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &tm);
  return out;
}

// ~/.uagent/<sub>, created on demand. user_home() falls back to the account
// database when HOME is absent, never to a project-controlled directory.
inline std::string UagentDir(const char* sub) {
  std::string home = UserHome();
  std::string base =
      home.empty() ? "/tmp/uagent-" + std::to_string(getuid()) + "/.uagent"
                   : home + "/.uagent";
  std::string dir = base + "/" + sub;
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  chmod(base.c_str(), 0700);
  std::filesystem::create_directories(dir, ec);
  chmod(dir.c_str(), 0700);
  return dir;
}

inline void PruneArtifactTree(const std::string& dir, int64_t max_age_days,
                              int64_t max_files) {
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
                std::chrono::hours(24 * std::max(int64_t{1}, max_age_days));
  for (fs::recursive_directory_iterator
           it(dir, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
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

inline void MaintainArtifacts() {
  PruneArtifactTree(UagentDir("history"), EnvLong("UAGENT_HISTORY_DAYS", 30),
                    EnvLong("UAGENT_HISTORY_FILES", 200));
  PruneArtifactTree(UagentDir("sessions"), EnvLong("UAGENT_DEBUG_DAYS", 14),
                    EnvLong("UAGENT_DEBUG_FILES", 50));
  PruneArtifactTree(UagentDir("bg"), EnvLong("UAGENT_BG_DAYS", 7),
                    EnvLong("UAGENT_BG_FILES", 200));
  PruneArtifactTree(UagentDir("mcp"), EnvLong("UAGENT_MCP_LOG_DAYS", 7),
                    EnvLong("UAGENT_MCP_LOG_FILES", 100));
}

inline uint64_t Fnv1aUpdate(uint64_t hash, const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline std::string Hex64(uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

inline std::string SafeFileComponent(std::string value) {
  for (char& c : value) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      c = '_';
    }
  }
  if (value.empty()) value = "unnamed";
  if (value.size() > 80) value.resize(80);
  return value;
}

inline std::string CanonicalCwd() {
  std::error_code ec;
  auto path =
      std::filesystem::weakly_canonical(std::filesystem::current_path(), ec);
  return ec ? std::filesystem::current_path().string() : path.string();
}

struct ProjectInstructions {
  std::string text;
  std::vector<std::string> sources;
  bool truncated = false;
};

inline std::filesystem::path ProjectRoot(const std::filesystem::path& cwd) {
  for (auto path = cwd;; path = path.parent_path()) {
    std::error_code ec;
    if (std::filesystem::exists(path / ".git", ec)) return path;
    if (path == path.root_path() || path.parent_path() == path) break;
  }
  return cwd;
}

// Codex-style startup discovery: one AGENTS override/default per directory,
// ordered from repository root to cwd. CLAUDE.md is additionally loaded at
// each level so mixed-tool repositories keep both instruction surfaces.
inline ProjectInstructions LoadProjectInstructions(
    const std::filesystem::path& cwd, size_t max_bytes) {
  namespace fs = std::filesystem;
  ProjectInstructions loaded;
  if (max_bytes == 0) return loaded;

  std::vector<fs::path> dirs;
  fs::path root = ProjectRoot(cwd);
  for (fs::path path = cwd;; path = path.parent_path()) {
    dirs.push_back(path);
    if (path == root || path == path.root_path() ||
        path.parent_path() == path) {
      break;
    }
  }
  std::reverse(dirs.begin(), dirs.end());

  size_t used = 0;
  auto append = [&](const fs::path& path) {
    if (used >= max_bytes) {
      loaded.truncated = true;
      return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    size_t remaining = max_bytes - used;
    std::string content(remaining + 1, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    size_t read = static_cast<size_t>(input.gcount());
    content.resize(std::min(read, remaining));
    if (read > remaining || input.peek() != std::char_traits<char>::eof()) {
      loaded.truncated = true;
    }
    content = Utf8Prefix(std::move(content), remaining);
    if (Trim(content).empty()) return false;
    if (!loaded.text.empty()) loaded.text += "\n\n";
    used += content.size();
    loaded.text += content;
    loaded.sources.push_back(path.string());
    return true;
  };
  // One file per directory, as Codex does. CLAUDE.md is a compatibility
  // fallback, not a second surface loaded alongside AGENTS.md.
  auto append_dir = [&](const fs::path& dir) {
    for (const char* name : {"AGENTS.override.md", "AGENTS.md", "CLAUDE.md"}) {
      std::error_code ec;
      fs::path candidate = dir / name;
      if (fs::is_regular_file(candidate, ec)) {
        append(candidate);
        break;
      }
    }
  };

  std::error_code ec;
  fs::path global = fs::weakly_canonical(UagentDir(""), ec);
  if (ec) global = UagentDir("");
  append_dir(global);
  for (const fs::path& dir : dirs) {
    if (dir != global) append_dir(dir);
  }
  return loaded;
}

inline std::string UrlHost(std::string url) {
  if (auto p = url.find("://"); p != std::string::npos) {
    url = url.substr(p + 3);
  }
  if (auto p = url.find('/'); p != std::string::npos) {
    url.resize(p);
  }
  if (auto p = url.rfind('@'); p != std::string::npos) {
    url = url.substr(p + 1);
  }
  if (auto p = url.find(':'); p != std::string::npos) {
    url.resize(p);
  }
  std::transform(url.begin(), url.end(), url.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return url;
}

inline bool OpenrouterUrl(std::string url) {
  url = UrlHost(std::move(url));
  constexpr const char* kSuffix = ".openrouter.ai";
  return url == "openrouter.ai" || (url.size() > strlen(kSuffix) &&
                                    url.compare(url.size() - strlen(kSuffix),
                                                strlen(kSuffix), kSuffix) == 0);
}

inline bool OpenaiUrl(std::string url) {
  return UrlHost(std::move(url)) == "api.openai.com";
}

inline std::string MakeSessionId() {
  static std::atomic<uint64_t> sequence{0};
  std::string seed =
      CanonicalCwd() + ":" + std::to_string(getpid()) + ":" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      ":" + std::to_string(++sequence);
  return "uagent-" +
         Hex64(Fnv1aUpdate(1469598103934665603ULL, seed.data(), seed.size()));
}

inline std::string WorkspaceId(const std::string& root) {
  return Hex64(Fnv1aUpdate(1469598103934665603ULL, root.data(), root.size()));
}

inline bool ProjectConfigPresent() {
  std::error_code ec;
  return std::filesystem::is_regular_file(".mcp.json", ec);
}

inline bool ProjectConfigSnapshot(json& snapshot, std::string& error) {
  std::error_code ec;
  uintmax_t bytes = std::filesystem::file_size(".mcp.json", ec);
  int64_t max_bytes =
      std::max(int64_t{1024}, EnvLong("UAGENT_MCP_CONFIG_BYTES", 1024 * 1024));
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

inline std::string TrustStorePath() {
  return UagentDir("config") + "/trusted-projects.json";
}

inline json ReadTrustStore() {
  std::ifstream file(TrustStorePath());
  if (!file) return json::object();
  json value = json::parse(file, nullptr, false);
  return value.is_object() ? value : json::object();
}

inline bool ProjectConfigTrusted(json* trusted_snapshot = nullptr) {
  json store = ReadTrustStore();
  std::string root = CanonicalCwd();
  json snapshot;
  std::string error;
  bool trusted =
      ProjectConfigSnapshot(snapshot, error) && store.contains(root) &&
      store[root].is_object() && JsonValue(store[root], "format", 0) == 2 &&
      store[root].contains("config") && store[root]["config"] == snapshot;
  if (trusted && trusted_snapshot) *trusted_snapshot = std::move(snapshot);
  return trusted;
}

inline bool TrustProjectConfig(std::string& error,
                               json* trusted_snapshot = nullptr) {
  json snapshot;
  if (!ProjectConfigSnapshot(snapshot, error)) return false;
  json store = ReadTrustStore();
  store[CanonicalCwd()] = {{"format", 2}, {"config", snapshot}};
  std::string path = TrustStorePath();
  std::string data = JsonDump(store, 2) + "\n";
  if (!AtomicWriteFile(path, data, 0600, /*preserve_mode=*/false, error)) {
    return false;
  }
  if (trusted_snapshot) *trusted_snapshot = std::move(snapshot);
  return true;
}

inline bool AppendPrivateLine(const std::string& path, const std::string& line,
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
inline std::string UsageLedger() {
  return UagentDir("bg") + "/usage-" + std::to_string(getpid()) + ".jsonl";
}

inline std::string DefaultDebugPath() {
  return UagentDir("sessions") + "/" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
         std::to_string(getpid()) + ".jsonl";
}

class DebugLog {
 public:
  ~DebugLog() {
    if (file_) fclose(file_);
  }

  bool Start(std::string path = "") {
    if (path.empty() || path == "1") path = DefaultDebugPath();
    if (path.starts_with("~/")) {
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
    int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
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

  bool Enabled() const { return file_ != nullptr; }
  const std::string& Path() const { return path_; }
  const std::string& Error() const { return error_; }

  void Write(const std::string& event, json data = json::object()) noexcept {
    if (!file_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    json record = {{"seq", ++seq_},
                   {"time", UtcStamp()},
                   {"elapsed_ms", ElapsedMs(started_)},
                   {"event", event},
                   {"data", std::move(data)}};
    std::string line = JsonDump(record);
    fwrite(line.data(), 1, line.size(), file_);
    fputc('\n', file_);
    fflush(file_);
  }

 private:
  FILE* file_ = nullptr;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point started_;
  std::string path_, error_;
  int64_t seq_ = 0;
};

inline DebugLog g_debug;

// compact debug logging helper — guards the enabled() check + alloc avoidance
inline void DebugLog(const std::string& event, json data = json::object()) {
  if (g_debug.Enabled()) g_debug.Write(event, std::move(data));
}

// --- immediate steering ------------------------------------------------------
// Bare ESC cancels active work; the next prompt either submits a replacement
// message with Enter or cancels with ESC.

class Steering {
 public:
  ~Steering() { Stop(); }

  bool Start() {
    if (!g_tty || !isatty(STDIN_FILENO) || !SteeringEnabled() ||
        running_.exchange(true)) {
      return false;
    }
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
    thread_ = std::thread([this] { Listen(); });
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    Restore();
  }

  bool Requested() const { return requested_; }

  bool Take() {
    bool value = requested_.exchange(false);
    ClearAbort();
    DebugLog("steering_take");
    return value;
  }

 private:
  void Restore() {
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

  void Listen() {
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
        RequestAbort();
        DebugLog("steering_interrupt");
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
  explicit SteeringGuard(bool enabled = true)
      : active_(enabled && g_steering.Start()) {}
  ~SteeringGuard() { Stop(); }
  void Stop() {
    if (!active_) return;
    g_steering.Stop();
    active_ = false;
  }

 private:
  bool active_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UTIL_H_
