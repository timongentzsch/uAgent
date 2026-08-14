// Copyright 2026 Timon Gentzsch

#include <signal.h>
#include <unistd.h>

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "include/app/bootstrap.h"
#include "include/app/options.h"
#include "include/cli.h"
#include "include/core/debug.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/term.h"
#include "include/core/usage.h"
#include "include/tools/jobs.h"

namespace uagent {
namespace {

void InitializeProcess() {
  std::setlocale(LC_CTYPE, "");
  const char* session = getenv("PLAYWRIGHT_CLI_SESSION");
  const char* generated = getenv("UAGENT_INTERNAL_PLAYWRIGHT_SESSION");
  if (!session || (generated && session == std::string(generated))) {
    std::string session = "uagent-" + std::to_string(getpid());
    setenv("PLAYWRIGHT_CLI_SESSION", session.c_str(), 1);
    setenv("UAGENT_INTERNAL_PLAYWRIGHT_SESSION", session.c_str(), 1);
  }
  g_tty = isatty(STDOUT_FILENO);
  g_signal_tty = g_tty;
  signal(SIGINT, SigintHandler);
  signal(SIGTERM, SigintHandler);
  signal(SIGHUP, SigintHandler);
  signal(SIGPIPE, SIG_IGN);
  InstallSigwinchHandler();
}

// Report a startup failure in whichever shape the caller asked for. Emit is a
// no-op when the event stream never started, so no extra guard is needed.
int Fail(const Options& options, const std::string& error, int exit_code) {
  json envelope = HeadlessResult("", error, json::array(), Usage{},
                                 json::object(), exit_code);
  if (options.json_stream) {
    Events().Emit("error", std::move(envelope));
  } else if (options.json) {
    printf("%s\n", JsonDump(envelope).c_str());
  } else {
    fprintf(stderr, "%s\n", error.c_str());
  }
  return exit_code;
}

}  // namespace

int Main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--log-pump") {
    int64_t bytes = 0;
    return ParseInt64(argv[3], bytes) && bytes >= 1024
               ? ToolLogPump(argv[2], bytes)
               : 2;
  }
  InitializeProcess();
  ParsedOptions parsed = ParseOptions(argc, argv);
  if (!parsed.Ok()) {
    if (parsed.options.json_stream) Events().Start();
    return Fail(parsed.options, parsed.error, 2);
  }
  if (parsed.action == OptionsAction::kHelp) {
    printf("%s", UsageText());
    return 0;
  }
  if (parsed.action == OptionsAction::kVersion) {
    printf("uagent %s\n", kVersion);
    return 0;
  }

  const Options reporting = parsed.options;
  if (reporting.json_stream && !Events().Start()) {
    fprintf(stderr, "cannot initialize JSON event stream\n");
    return 1;
  }
  BootstrapResult boot = Bootstrap(std::move(parsed.options), argv[0]);
  if (!boot.Ok()) return Fail(reporting, boot.error, boot.exit_code);
  return RunApplication(*boot.context);
}

}  // namespace uagent

int main(int argc, char** argv) { return uagent::Main(argc, argv); }
