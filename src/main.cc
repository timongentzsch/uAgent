// Copyright 2026 Timon Gentzsch

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern char** environ;

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "include/agent/activity.h"
#include "include/agent/session_store.h"
#include "include/app/bootstrap.h"
#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/execution.h"
#include "include/app/session.h"
#include "include/app/setup.h"
#include "include/browser/browser.h"
#include "include/cli/cli.h"
#include "include/cli/options.h"
#include "include/core/build.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/events.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/usage.h"
#include "include/tools/memory.h"
#include "include/web/protocol.h"
#include "include/web/remote_hosts.h"

namespace uagent {
namespace {

// Defences for a process that holds API keys in memory: no core files, no
// same-user ptrace where the platform offers it, no loader-injection
// variables for children. PT_DENY_ATTACH is not used: it breaks debuggers.
void HardenProcess() {
  rlimit no_core{};
  setrlimit(RLIMIT_CORE, &no_core);
#ifdef PR_SET_DUMPABLE
  prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
  static constexpr std::string_view kInjectionPrefixes[] = {"LD_", "DYLD_"};
  std::vector<std::string> remove;
  for (char** entry = environ; entry && *entry; ++entry) {
    std::string_view variable(*entry);
    size_t equals = variable.find('=');
    if (equals == std::string_view::npos) continue;
    std::string_view key = variable.substr(0, equals);
    for (std::string_view prefix : kInjectionPrefixes) {
      if (key.starts_with(prefix)) remove.emplace_back(key);
    }
  }
  for (const std::string& key : remove) unsetenv(key.c_str());
}

void InitializeProcess() {
  HardenProcess();
  std::setlocale(LC_CTYPE, "");
  g_tty = isatty(STDOUT_FILENO);
  g_color = ResolveColorEnabled(g_tty);
  g_attributes = ResolveAttributesEnabled(g_tty);
  // Without a multibyte locale every non-ASCII character measures zero
  // columns, so glyphs are only safe to emit once one is in effect.
  g_unicode = EnsureUtf8Ctype() && ResolveUnicodeEnabled();
  g_signal_tty = g_tty;
  InitializeSignalNotifications();
  signal(SIGINT, SigintHandler);
  signal(SIGTERM, SigintHandler);
  signal(SIGHUP, SigintHandler);
  signal(SIGPIPE, SIG_IGN);
  InstallSigchldHandler();
  InstallSigwinchHandler();
  InstallSuspendHandlers();
}

// The terminal profile the user configured, or asked for with --plain.
void ResolveTerminalProfile(const Options& options) {
  const auto settings = ConfigManager::Capture(options.overrides).Read();
  auto on = [&](const char* key) {
    bool value = false;
    ParseBool(settings.Get(key), value);
    return value;
  };
  ApplyTerminalProfile(on("UAGENT_PLAIN"));
}

// Emit is a no-op when the event stream never started.
int Fail(bool json_stream, bool json_envelope, const std::string& error,
         int exit_code) {
  json envelope = HeadlessResult("", error, json::array(), Usage{},
                                 json::object(), exit_code);
  if (json_stream) {
    Emit(Event{EventId::kError, std::move(envelope)});
  } else if (json_envelope) {
    printf("%s\n", JsonDump(envelope).c_str());
  } else {
    fprintf(stderr, "%s\n", error.c_str());
  }
  return exit_code;
}

// --show-system-prompt: builds the session far enough to print its prompt.
int ShowSystemPrompt(Options options, char* executable,
                     Observability& observability) {
  if (options.web || !options.prompt.empty() || options.json_stream ||
      options.resume_latest || options.resume_pick ||
      !options.attach_paths.empty()) {
    fprintf(stderr,
            "--show-system-prompt is a standalone inspection command\n");
    return 2;
  }
  const bool structured = options.json;
  HeadlessOutput silence;
  if (!silence.Silence()) return 1;
  auto boot = Bootstrap(std::move(options), executable, observability);
  json result = boot.Ok() ? boot.context->agent->PromptPreview()
                          : json{{"error", boot.error}};
  boot.context.reset();
  silence.Restore();
  printf("%s\n",
         structured
             ? JsonDump(result, 2).c_str()
             : JsonValue(result,
                         result.contains("error") ? "error" : "effective", "")
                   .c_str());
  return result.contains("error") ? 1 : 0;
}

// --web: the master process; only user configuration reaches it.
int WebMain(const Options& options, char** argv) {
  if (!options.prompt.empty() || options.json || options.json_stream ||
      options.resume_latest || options.resume_pick || options.yolo ||
      options.trust_project || !options.attach_paths.empty()) {
    fprintf(stderr, "--web cannot inherit session execution options\n");
    return 2;
  }
  for (const auto& [key, value] : options.overrides) {
    if (key != "UAGENT_WEB_PORT" && key != "UAGENT_WEB_ORIGIN") {
      fprintf(stderr,
              "configure each web session through its own folder or "
              "controls\n");
      return 2;
    }
  }
  auto settings = ConfigManager::Capture(options.overrides, "").Read();
  auto setting = [&](const char* key, std::string_view fallback = {}) {
    auto found = settings.values.find(key);
    return found == settings.values.end() ? std::string(fallback)
                                          : found->second;
  };
  // The registry declares the default and the range. A port outside it is
  // refused here: a clamped one would listen where nobody asked.
  const ConfigDescriptor& range = Cfg("UAGENT_WEB_PORT");
  int64_t port = std::get<int64_t>(range.default_value);
  const auto saved = settings.values.find("UAGENT_WEB_PORT");
  if ((saved != settings.values.end() &&
       !ParseInt64(saved->second.c_str(), port)) ||
      port < range.minimum || port > range.maximum) {
    fprintf(stderr, "web port must be between %s and %s\n",
            std::to_string(range.minimum).c_str(),
            std::to_string(range.maximum).c_str());
    return 2;
  }
  std::string bind =
      setting("UAGENT_WEB_BIND",
              std::get<std::string_view>(Cfg("UAGENT_WEB_BIND").default_value));
  if (bind != "127.0.0.1" && bind != "0.0.0.0") {
    fprintf(stderr, "web bind must be 127.0.0.1 or 0.0.0.0\n");
    return 2;
  }
  const ConfigDescriptor& terminal = Cfg("UAGENT_WEB_TERMINAL");
  bool terminal_on = std::get<bool>(terminal.default_value);
  ParseBool(setting("UAGENT_WEB_TERMINAL"), terminal_on);
  const ConfigDescriptor& detail = Cfg("UAGENT_WEB_NOTIFICATION_DETAIL");
  bool notification_detail = std::get<bool>(detail.default_value);
  ParseBool(setting("UAGENT_WEB_NOTIFICATION_DETAIL"), notification_detail);
  const ConfigDescriptor& idle = Cfg("UAGENT_WEB_TERMINAL_IDLE_MINUTES");
  int64_t idle_minutes = std::get<int64_t>(idle.default_value);
  ParseInt64(setting("UAGENT_WEB_TERMINAL_IDLE_MINUTES").c_str(), idle_minutes);
  bool update_check = false;
  ParseBool(setting("UAGENT_WEB_UPDATE_CHECK"), update_check);
  return web::MasterMain(
      {static_cast<int>(port), setting("UAGENT_WEB_ORIGIN"),
       setting("UAGENT_WEB_PUSH_CONTACT"), notification_detail, bind,
       setting("UAGENT_BROWSER_DATA"), terminal_on,
       std::clamp(idle_minutes, idle.minimum, idle.maximum),
       setting("UAGENT_REMOTE_BUNDLE"), update_check},
      argv);
}

int HeadlessMain(Options options, char* executable,
                 Observability& observability) {
  const bool json_stream = options.json_stream;
  const bool json_envelope = options.json;
  observability.EnableJournal(false);  // headless runs keep no journal
  if (json_stream && !observability.StartJsonStream()) {
    fprintf(stderr, "cannot initialize JSON event stream\n");
    return 1;
  }
  BootstrapResult boot =
      Bootstrap(std::move(options), executable, observability);
  int code = boot.Ok()
                 ? RunApplication(*boot.context)
                 : Fail(json_stream, json_envelope, boot.error, boot.exit_code);
  // Owners stop in reverse order: runtime, then sinks, then signal state.
  boot.context.reset();
  observability.Shutdown();
  return code;
}

}  // namespace

int Main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--build-id") {
    printf("%s\n", std::string(BuildIdentity()).c_str());
    return 0;
  }
  // Both hidden modes are re-execs that run before any startup below: the
  // sandbox trampoline has to confine itself while it is alone in the process.
  if (argc > 2 && std::string(argv[1]) == "--sandbox-child") {
    return SandboxChildMain(argc, argv);
  }
  if (argc == 4 && std::string(argv[1]) == "--log-pump") {
    int64_t bytes = 0;
    return ParseInt64(argv[3], bytes) && bytes >= 1024
               ? ToolLogPump(argv[2], bytes)
               : 2;
  }
  InitializeProcess();
  SetExecutablePath(argv[0]);
  if (argc > 1 && (std::string_view(argv[1]) == "--execution-start" ||
                   std::string_view(argv[1]) == "--execution-worker")) {
    return execution::Main(argc, argv);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--check-remote-bundle") {
    std::string error;
    if (web::CheckRemoteBundle(argv[2], error)) return 0;
    fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--session-worker") {
    return session::WorkerMain(argc, argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "setup") {
    return SetupMain(argc, argv);
  }
  if (argc == 2 && std::string_view(argv[1]) == "--browser-service") {
    return browser::ServiceMain(3);
  }
  Observability observability;
  SetObservability(&observability);
  ParsedOptions parsed = ParseOptions(argc, argv);
  if (!parsed.Ok()) {
    if (parsed.options.json_stream) observability.StartJsonStream();
    return Fail(parsed.options.json_stream, parsed.options.json, parsed.error,
                2);
  }
  if (parsed.action == OptionsAction::kHelp) {
    printf("%s", UsageText());
    return 0;
  }
  if (parsed.action == OptionsAction::kPrintVersion) {
    printf("uagent %s\n", kVersion);
    return 0;
  }
  if (!parsed.options.control.empty()) {
    return ControlMain(parsed.options.control);
  }
  if (parsed.options.show_system_prompt) {
    return ShowSystemPrompt(std::move(parsed.options), argv[0], observability);
  }
  if (parsed.options.web) return WebMain(parsed.options, argv);
  if (parsed.options.prompt.empty() && !parsed.options.json &&
      !parsed.options.json_stream) {
    ResolveTerminalProfile(parsed.options);
    return session::TerminalMain(std::move(parsed.options));
  }
  return HeadlessMain(std::move(parsed.options), argv[0], observability);
}

}  // namespace uagent

int main(int argc, char** argv) { return uagent::Main(argc, argv); }
