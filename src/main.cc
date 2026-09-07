// Copyright 2026 Timon Gentzsch

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern char** environ;

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/app/bootstrap.h"
#include "include/app/options.h"
#include "include/app/reference.h"
#include "include/cli.h"
#include "include/core/events.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/term.h"
#include "include/core/usage.h"
#include "include/tools/jobs.h"

namespace uagent {
namespace {

// Cheap, once-per-process defences for a tool that holds API keys in memory:
// keep the heap out of core files, block same-user ptrace where the platform
// offers it for free, and drop loader-injection variables before any child
// process inherits them. None of this costs anything at run time.
//
// macOS PT_DENY_ATTACH is deliberately not used: it would break running µAgent
// under a debugger, which matters for a tool people build from source.
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
  const char* session = getenv("PLAYWRIGHT_CLI_SESSION");
  const char* generated = getenv("UAGENT_INTERNAL_PLAYWRIGHT_SESSION");
  if (!session || (generated && session == std::string(generated))) {
    const std::string owned = "uagent-" + std::to_string(getpid());
    setenv("PLAYWRIGHT_CLI_SESSION", owned.c_str(), 1);
    setenv("UAGENT_INTERNAL_PLAYWRIGHT_SESSION", owned.c_str(), 1);
  }
  g_tty = isatty(STDOUT_FILENO);
  g_color = ResolveColorEnabled(g_tty);
  g_unicode = ResolveUnicodeEnabled();
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

// Report a startup failure in whichever shape the caller asked for. Emit is a
// no-op when the event stream never started, so no extra guard is needed.
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

}  // namespace

int Main(int argc, char** argv) {
  // Both hidden modes are re-execs of this binary that must do their one job
  // before any of the startup below runs: the sandbox trampoline in particular
  // has to confine itself while it is still the only thing in the process.
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
  if (parsed.action == OptionsAction::kEmitReference) {
    std::string error;
    if (!WriteReferenceFiles(parsed.options.reference_dir, error)) {
      fprintf(stderr, "uagent: %s\n", error.c_str());
      return 1;
    }
    return 0;
  }

  const bool json_stream = parsed.options.json_stream;
  const bool json_envelope = parsed.options.json;
  observability.EnableJournal(parsed.options.prompt.empty());
  if (json_stream && !observability.StartJsonStream()) {
    fprintf(stderr, "cannot initialize JSON event stream\n");
    return 1;
  }
  BootstrapResult boot =
      Bootstrap(std::move(parsed.options), argv[0], observability);
  if (!boot.Ok()) {
    int code = Fail(json_stream, json_envelope, boot.error, boot.exit_code);
    boot.context.reset();
    observability.Shutdown();
    return code;
  }
  int code = RunApplication(*boot.context);
  // Direct owners stop in deterministic reverse order: application/runtime,
  // then observational sinks, then process-level signal state at exit.
  boot.context.reset();
  observability.Shutdown();
  return code;
}

}  // namespace uagent

int main(int argc, char** argv) { return uagent::Main(argc, argv); }
