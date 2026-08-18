// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>

#include "include/agent.h"
#include "include/agent/protocol.h"
#include "include/api/stream.h"
#include "include/core/events.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/tools/registry.h"

namespace uagent {
namespace {

volatile size_t sink = 0;

template <class F>
double Measure(size_t iterations, F&& work) {
  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) sink += work();
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void Report(const char* name, size_t iterations, double milliseconds) {
  double operations_per_second =
      milliseconds > 0 ? static_cast<double>(iterations) * 1000.0 / milliseconds
                       : 0;
  std::cout << std::left << std::setw(28) << name << std::right << std::fixed
            << std::setprecision(2) << std::setw(10) << milliseconds << " ms  "
            << std::setprecision(0) << std::setw(12) << operations_per_second
            << " ops/s\n";
}

void BenchmarkStream(bool tty, bool render = true) {
  bool prior_tty = g_tty;
  g_tty = tty;
  ChatResult result;
  StreamCtx stream;
  stream.res = &result;
  stream.status = 200;
  stream.started = std::chrono::steady_clock::now();
  // Rendering is no longer a StreamCtx flag: the stream emits observation
  // events and the terminal presenter owned by Observability draws them.
  Observability observability;
  SetObservability(&observability);
  const std::string event =
      "data: {\"choices\":[{\"delta\":{\"content\":"
      "\"stream benchmark payload \"},\"finish_reason\":null}]}\n\n";
  constexpr size_t kEvents = 10000;

  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  int null = open("/dev/null", O_WRONLY);
  if (saved < 0 || null < 0 || dup2(null, STDOUT_FILENO) < 0) {
    if (null >= 0) close(null);
    if (saved >= 0) close(saved);
    g_tty = prior_tty;
    perror("cannot redirect benchmark output");
    return;
  }
  close(null);
  double milliseconds = 0;
  {
    ResponseObservation response(render, /*verbose=*/false, "benchmark");
    milliseconds = Measure(kEvents, [&] {
      size_t fed = stream.Feed(event.data(), event.size());
      return fed == event.size() ? size_t{1} : size_t{0};
    });
  }
  observability.Shutdown();
  SetObservability(nullptr);
  fflush(stdout);
  if (dup2(saved, STDOUT_FILENO) < 0) {
    close(saved);
    g_tty = prior_tty;
    perror("cannot restore benchmark output");
    return;
  }
  close(saved);
  g_tty = prior_tty;

  const char* name = !render ? "SSE + headless"
                     : tty   ? "SSE + TTY Markdown"
                             : "SSE + plain";
  Report(name, kEvents, milliseconds);
}

}  // namespace

int RunBenchmarks() {
  constexpr size_t kIterations = 10000;
  const std::string call =
      "[uagent_tool_call]{\"name\":\"read_file\",\"arguments\":{\"path\":\"src/"
      "main.cc\","
      "\"offset\":1,\"limit\":200}}[/uagent_tool_call]";
  const std::string hostile = "normal text\n\x1b]52;c;payload\x07\tmore text\n";
  const std::string large(24000, 'x');

  Report("text tool-call parse", kIterations,
         Measure(kIterations, [&] { return ParseTextToolCalls(call).size(); }));

  bool prior_tty = g_tty;
  g_tty = true;
  Report("terminal sanitization", kIterations,
         Measure(kIterations, [&] { return TerminalSafe(hostile).size(); }));
  g_tty = prior_tty;

  Report("tool-result cap", kIterations,
         Measure(kIterations, [&] { return CapResult(large).size(); }));

  BenchmarkStream(false);
  BenchmarkStream(true);
  BenchmarkStream(false, false);

  ProcessSupervisor processes;
  auto lean_tools = BuiltinTools(processes, CanonicalAccessPath("."), false);
  auto image_tools = BuiltinTools(processes, CanonicalAccessPath("."), true);
  auto without = [](std::vector<Tool> tools, const std::string& name) {
    std::erase_if(tools, [&](const Tool& tool) { return tool.name == name; });
    return tools;
  };
  auto no_scratch_tools = without(lean_tools, "scratch");
  auto no_edit_tools = without(lean_tools, "edit_file");
  auto base_tools = without(no_scratch_tools, "grep");
  size_t base_schema = ToolSchemas(base_tools).dump().size();
  size_t grep_schema = ToolSchemas(no_scratch_tools).dump().size();
  size_t no_edit_schema = ToolSchemas(no_edit_tools).dump().size();
  size_t lean_schema = ToolSchemas(lean_tools).dump().size();
  size_t image_schema = ToolSchemas(image_tools).dump().size();
  std::cout << "built-in schema              " << lean_schema << " bytes (~"
            << lean_schema / 4 << " tokens); grep adds "
            << grep_schema - base_schema << " bytes; scratch adds "
            << lean_schema - grep_schema << " bytes; edit adds "
            << lean_schema - no_edit_schema << " bytes; inline image adds "
            << image_schema - lean_schema << " bytes\n";
  for (const Tool& tool : lean_tools) {
    std::cout << "  " << std::left << std::setw(24) << tool.name << std::right
              << JsonDump(ToolSchema(tool)).size() << " bytes\n";
  }
  std::cout << "sink " << sink << '\n';
  return 0;
}

}  // namespace uagent

int main() { return uagent::RunBenchmarks(); }
