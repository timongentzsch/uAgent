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
#include "include/core/signals.h"
#include "include/core/term.h"
#include "include/tools/jobs.h"

namespace uagent {
namespace {

void InitializeProcess() {
  std::setlocale(LC_CTYPE, "");
  g_tty = isatty(STDOUT_FILENO);
  g_signal_tty = g_tty;
  signal(SIGINT, SigintHandler);
  signal(SIGTERM, SigintHandler);
  signal(SIGHUP, SigintHandler);
  signal(SIGPIPE, SIG_IGN);
#if defined(HAVE_EDITLINE)
  rl_getc_function = EscGetc;
#endif
}

}  // namespace

int Main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--log-pump") {
    char* end = nullptr;
    int64_t bytes = strtol(argv[3], &end, 10);
    return end && *end == '\0' && bytes >= 1024 ? ToolLogPump(argv[2], bytes)
                                                : 2;
  }
  InitializeProcess();
  ParsedOptions parsed = ParseOptions(argc, argv);
  if (!parsed.Ok()) {
    fprintf(stderr, "%s\n", parsed.error.c_str());
    return 2;
  }
  if (parsed.action == OptionsAction::kHelp) {
    printf("%s", UsageText());
    return 0;
  }
  if (parsed.action == OptionsAction::kVersion) {
    printf("uagent %s\n", kVersion);
    return 0;
  }

  BootstrapResult boot = Bootstrap(std::move(parsed.options), argv[0]);
  if (!boot.Ok()) {
    fprintf(stderr, "%s\n", boot.error.c_str());
    return boot.exit_code;
  }
  return RunApplication(*boot.context);
}

}  // namespace uagent

int main(int argc, char** argv) { return uagent::Main(argc, argv); }
