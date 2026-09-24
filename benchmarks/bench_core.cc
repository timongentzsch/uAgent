// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "include/agent.h"
#include "include/agent/protocol.h"
#include "include/api.h"
#include "include/api/stream.h"
#include "include/api/wire.h"
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

// Streamed deltas are observation events; clients render them elsewhere, so
// this measures decoding plus event delivery in the runtime process.
void BenchmarkStream() {
  ChatResult result;
  StreamCtx stream;
  stream.res = &result;
  stream.status = 200;
  stream.started = std::chrono::steady_clock::now();
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
    perror("cannot redirect benchmark output");
    return;
  }
  close(null);
  double milliseconds = 0;
  {
    ResponseObservation response(/*verbose=*/false, "benchmark");
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
    perror("cannot restore benchmark output");
    return;
  }
  close(saved);
  Report("SSE + headless", kEvents, milliseconds);
}

// Burst benchmark through actual event delivery, comparing identical main
// and two child workloads with activity projection inactive/active. No wire
// or browser/network timing is implied by this native measurement.
void BenchmarkActivity(bool labels) {
  constexpr size_t kSessions = 3;
  constexpr size_t kTokens = 10000;
  const auto started = std::chrono::steady_clock::now();
  const auto cpu_started = std::clock();
  std::vector<std::future<size_t>> workers;
  workers.reserve(kSessions);
  for (size_t session = 0; session < kSessions; ++session) {
    workers.push_back(std::async(std::launch::async, [labels] {
      Observability events;
      size_t captions = 0;
      events.Subscribe([&](const AppEvent& event) {
        if (event.type == "activity.status") ++captions;
      });
      if (labels) events.Emit(Event{EventId::kTurnStarted, {{"turn", 1}}});
      events.Emit(Event{EventId::kResponseStarted,
                        {{"turn", 1}, {"response_id", "r"}}});
      for (size_t index = 0; index < kTokens; ++index) {
        events.Emit(Event{
            EventId::kReasoningDelta,
            {{"response_id", "r"},
             {"text", index % 1000 == 0 ? "\nChecking tests\n" : "token "}}});
        if (index % 1000 == 0) {
          for (const auto* call : {"a", "b"}) {
            events.Emit(Event{EventId::kToolStarted,
                              {{"response_id", "r"},
                               {"occurrence_id", call},
                               {"name", "read_path"}}});
          }
          for (const auto* call : {"a", "b"}) {
            events.Emit(Event{EventId::kToolResult,
                              {{"response_id", "r"}, {"occurrence_id", call}}});
          }
        }
      }
      events.Emit(Event{EventId::kTurnCompleted, {{"turn", 1}}});
      return captions;
    }));
  }
  size_t captions = 0;
  for (auto& worker : workers) captions += worker.get();
  Report(labels ? "3 sessions + captions" : "3 sessions baseline",
         kSessions * kTokens, ElapsedMs(started));
  std::cout << "  status events " << captions << "; process CPU "
            << 1000.0 * static_cast<double>(std::clock() - cpu_started) /
                   CLOCKS_PER_SEC
            << " ms\n";
}

}  // namespace

int RunBenchmarks() {
  constexpr size_t kIterations = 10000;
  const std::string hostile = "normal text\n\x1b]52;c;payload\x07\tmore text\n";
  const std::string large(24000, 'x');

  bool prior_tty = g_tty;
  g_tty = true;
  Report("terminal sanitization", kIterations,
         Measure(kIterations, [&] { return TerminalSafe(hostile).size(); }));
  g_tty = prior_tty;

  Report("tool-result cap", kIterations,
         Measure(kIterations, [&] { return CapResult(large).size(); }));

  BenchmarkStream();
  BenchmarkActivity(false);
  BenchmarkActivity(true);

  json history = json::array();
  for (size_t index = 0; index < 128; ++index) {
    history.push_back({{"role", index % 2 == 0 ? "user" : "assistant"},
                       {"content", std::string(256, 'x')}});
  }
  const json no_tools = json::array();
  const std::string benchmark_model = "benchmark-model";
  constexpr size_t kWireIterations = 1000;
  auto wire_benchmark = [&](WireApi wire_api, const char* name) {
    WireRequest request{.model = benchmark_model,
                        .messages = history,
                        .tool_schemas = no_tools,
                        .reasoning_effort = "high",
                        .max_output_tokens = 4096,
                        .stream_usage = true};
    Report(name, kWireIterations, Measure(kWireIterations, [&] {
             return JsonDump(EncodeWireRequest(wire_api, request)).size();
           }));
  };
  wire_benchmark(WireApi::kChatCompletions, "encode Chat Completions");
  wire_benchmark(WireApi::kResponses, "encode Responses");
  wire_benchmark(WireApi::kAnthropicMessages, "encode Anthropic Messages");
  ProcessSupervisor processes;
  auto lean_tools = BuiltinTools(processes, CanonicalAccessPath("."));
  const json cached_tools = ToolSchemas(lean_tools);
  for (WireApi wire : {WireApi::kChatCompletions, WireApi::kResponses,
                       WireApi::kAnthropicMessages}) {
    Api cached;
    cached.model = benchmark_model;
    cached.capabilities.wire_api = wire;
    const std::string label = std::string(WireApiName(wire));
    size_t revision = 0;
    auto change_tail = [&] {
      history.back()["content"] =
          std::string(256, 'x') + std::to_string(revision++);
    };
    Report((label + " full payload").c_str(), kWireIterations,
           Measure(kWireIterations, [&] {
             change_tail();
             return JsonDump(cached.BuildRequestBody(history, cached_tools))
                 .size();
           }));
    revision = 0;
    (void)cached.ChatPayload(history, cached_tools);
    Report((label + " cached payload").c_str(), kWireIterations,
           Measure(kWireIterations, [&] {
             change_tail();
             return cached.ChatPayload(history, cached_tools).size();
           }));
  }

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
  auto without_descriptions = lean_tools;
  for (Tool& tool : without_descriptions) {
    if (tool.declared_intent) {
      tool.parameters["properties"].erase("description");
    }
  }
  std::cout << "optional action descriptions "
            << lean_schema - ToolSchemas(without_descriptions).dump().size()
            << " schema bytes\n";
  std::cout << "built-in schema              " << lean_schema
            << " bytes; grep adds " << grep_schema - base_schema
            << " bytes; scratch adds " << lean_schema - grep_schema
            << " bytes; edit adds " << lean_schema - no_edit_schema
            << " bytes\n";
  for (const Tool& tool : lean_tools) {
    std::cout << "  " << std::left << std::setw(24) << tool.name << std::right
              << JsonDump(ToolSchema(tool)).size() << " bytes\n";
  }
  std::cout << "sink " << sink << '\n';
  return 0;
}

}  // namespace uagent

int main() { return uagent::RunBenchmarks(); }
