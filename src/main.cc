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
#include "include/app/eval.h"
#include "include/app/headless.h"
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
  g_tty = isatty(STDOUT_FILENO);
  g_signal_tty = g_tty;
  signal(SIGINT, SigintHandler);
  signal(SIGTERM, SigintHandler);
  signal(SIGHUP, SigintHandler);
  signal(SIGPIPE, SIG_IGN);
  struct sigaction resize_action{};
  resize_action.sa_handler = SigwinchHandler;
  sigemptyset(&resize_action.sa_mask);
  sigaction(SIGWINCH, &resize_action, nullptr);
  ConfigureLineEditor();
}

void PrintHeadlessFailure(const std::string& error, int exit_code) {
  json envelope = HeadlessResult("", error, json::array(), Usage{},
                                 json::object(), exit_code);
  printf("%s\n", JsonDump(envelope).c_str());
}

void PrintStreamFailure(const std::string& error, int exit_code) {
  Events().Emit("error", HeadlessResult("", error, json::array(), Usage{},
                                        json::object(), exit_code));
}

}  // namespace

int Main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--log-pump") {
    char* end = nullptr;
    int64_t bytes = strtol(argv[3], &end, 10);
    return end && *end == '\0' && bytes >= 1024 ? ToolLogPump(argv[2], bytes)
                                                : 2;
  }
  if (argc >= 2 && std::string(argv[1]) == "eval") {
    return RunEval(argc, argv);
  }
  InitializeProcess();
  ParsedOptions parsed = ParseOptions(argc, argv);
  if (!parsed.Ok()) {
    if (parsed.options.json_stream) {
      if (Events().Start()) PrintStreamFailure(parsed.error, 2);
    } else if (parsed.options.json) {
      PrintHeadlessFailure(parsed.error, 2);
    } else {
      fprintf(stderr, "%s\n", parsed.error.c_str());
    }
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

  bool json_output = parsed.options.json;
  bool json_stream = parsed.options.json_stream;
  if (json_stream && !Events().Start()) {
    fprintf(stderr, "cannot initialize JSON event stream\n");
    return 1;
  }
  BootstrapResult boot = Bootstrap(std::move(parsed.options), argv[0]);
  if (!boot.Ok()) {
    if (json_stream) {
      PrintStreamFailure(boot.error, boot.exit_code);
    } else if (json_output) {
      PrintHeadlessFailure(boot.error, boot.exit_code);
    } else {
      fprintf(stderr, "%s\n", boot.error.c_str());
    }
    return boot.exit_code;
  }
  return RunApplication(*boot.context);
}

}  // namespace uagent

int main(int argc, char** argv) { return uagent::Main(argc, argv); }
