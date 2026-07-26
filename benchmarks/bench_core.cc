// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>

#include "include/agent.h"
#include "include/util.h"

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

  ProcessSupervisor processes;
  auto lean_tools = BuiltinTools(processes, CanonicalAccessPath("."), false);
  auto image_tools = BuiltinTools(processes, CanonicalAccessPath("."), true);
  auto base_tools = lean_tools;
  auto without = [](std::vector<Tool> tools, const std::string& name) {
    std::erase_if(tools, [&](const Tool& tool) { return tool.name == name; });
    return tools;
  };
  auto no_python_tools = without(lean_tools, "run_python");
  base_tools = without(no_python_tools, "grep");
  size_t base_schema = ToolSchemas(base_tools).dump().size();
  size_t grep_schema = ToolSchemas(no_python_tools).dump().size();
  size_t lean_schema = ToolSchemas(lean_tools).dump().size();
  size_t image_schema = ToolSchemas(image_tools).dump().size();
  std::cout << "built-in schema              " << lean_schema << " bytes (~"
            << lean_schema / 4 << " tokens); grep adds "
            << grep_schema - base_schema << " bytes; Python adds "
            << lean_schema - grep_schema << " bytes; inline image adds "
            << image_schema - lean_schema << " bytes\n";
  std::cout << "sink " << sink << '\n';
  return 0;
}

}  // namespace uagent

int main() { return uagent::RunBenchmarks(); }
