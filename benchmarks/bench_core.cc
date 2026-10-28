// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/agent.h"
#include "include/agent/protocol.h"
#include "include/agent/session_view.h"
#include "include/api/client.h"
#include "include/api/stream.h"
#include "include/api/wire.h"
#include "include/app/replay_log.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/output_buffer.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/media/attachments.h"
#include "include/tools/registry.h"
#include "include/transport/session.h"
#include "include/ui/display.h"
#include "include/ui/input_decoder.h"
#include "include/ui/interactive.h"
#include "include/ui/live_region.h"
#include "include/ui/markdown.h"

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
    ResponseObservation response("benchmark");
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

// A 1 MB reasoning stream folded into a held view the way worker, host and
// terminal each do it: one delta per 12 ms batch of a fast model (64 bytes).
void BenchmarkStreamedAnswer() {
  constexpr size_t kAnswerBytes = MiB(1);
  constexpr size_t kBatchBytes = 64;
  const std::string chunk(kBatchBytes, 'x');
  const std::string delta_type(AppType(EventId::kAnswerDelta));
  json state;
  ApplySessionEvent(state, std::string(AppType(EventId::kResponseStarted)),
                    {{"response_id", "r"}});
  size_t offset = 0;
  Report("1 MB answer, 12 ms batches", kAnswerBytes / kBatchBytes,
         Measure(kAnswerBytes / kBatchBytes, [&] {
           json patch;
           ApplySessionEvent(
               state, delta_type,
               {{"response_id", "r"}, {"text", chunk}, {"offset", offset}},
               &patch);
           offset += kBatchBytes;
           return patch.size();
         }));
}

// 1 GB of command output (a long build or test log) through the bounded
// buffer a running command keeps, in the 8 KiB reads the process reader makes.
void BenchmarkCommandOutput() {
  constexpr size_t kOutputBytes = MiB(1024);
  const std::string read(KiB(8), 'x');
  HeadTailBuffer buffer;
  Report("1 GB command output", kOutputBytes / read.size(),
         Measure(kOutputBytes / read.size(), [&] {
           buffer.Push(read);
           return size_t{1};
         }));
}

// A checkpoint's 64 blocks merged into the 64 the host already holds.
void BenchmarkCheckpointMerge() {
  constexpr size_t kBlocks = 64;
  constexpr size_t kCheckpoints = 100;
  json checkpoint = json::array();
  for (size_t index = 1; index <= kBlocks; ++index) {
    checkpoint.push_back({{"id", "m-" + std::to_string(index)},
                          {"sequence", index},
                          {"kind", index % 2 ? "user" : "assistant"},
                          {"text", std::string(KiB(4), 'x')},
                          {"truncated", false}});
  }
  json view = json::object();
  MergeDisplayBlocks(view, checkpoint);
  Report("64-block checkpoint merge", kCheckpoints, Measure(kCheckpoints, [&] {
           MergeDisplayBlocks(view, checkpoint);
           return view["blocks"].size();
         }));
}

// An event stream reading the newest frames of a full replay log, as each
// connected device does after every publish.
void BenchmarkReplayRead() {
  constexpr size_t kHeld = 2048;
  constexpr size_t kReads = 10000;
  session::ReplayLog log(MiB(4), kHeld);
  for (size_t index = 0; index < kHeld; ++index) {
    log.Publish("e", "s", "g",
                {{"kind", "block"}, {"append", {{"text", "token "}}}}, false);
  }
  Report("replay read, 2048 held", kReads, Measure(kReads, [&] {
           return log.Read(log.Cursor() - 1, true, log.Cursor(), KiB(64))
               .events.size();
         }));
}

// A 4 MB PDF attached in a turn goes with every model call of that turn:
// 20 calls, each preparing the request from the stored conversation.
void BenchmarkAttachedDocument() {
  constexpr size_t kCalls = 20;
  ScopedTempFile pdf(
      (std::filesystem::temp_directory_path() / "uagent-bench-XXXXXX")
          .string());
  const std::string body = "%PDF-" + std::string(MiB(4), 'x');
  if (!pdf || write(pdf.Get(), body.data(), body.size()) !=
                  static_cast<ssize_t>(body.size())) {
    perror("cannot write benchmark document");
    return;
  }
  const json messages = {{{"role", "user"},
                          {"content",
                           {{{"type", "attachment"},
                             {"path", pdf.Path()},
                             {"name", "report.pdf"},
                             {"mime", "application/pdf"}}}}}};
  ProviderCapabilities capabilities;
  capabilities.file_input = true;
  Report("4 MB PDF, 20 model calls", kCalls, Measure(kCalls, [&] {
           json projected = messages;
           std::string error;
           PrepareAttachments(projected, capabilities, false, "bench", error);
           return JsonEstimatedBytes(projected);
         }));
}

// The terminal client's paint path at 100 columns, one paint per token:
// markdown renders the token into the stdout pipe, and the live region draws
// the tail, the working row and a draft naming a directory, whose listing the
// composer offers below it. One paragraph of 2,000 tokens, plain or with an
// inline code span every third token; terminal bytes go to a file.
void BenchmarkTerminalPaint(bool styled) {
  constexpr size_t kTokens = 2000;
  namespace fs = std::filesystem;
  const fs::path folder = fs::temp_directory_path() / "uagent-bench-paint";
  fs::create_directories(folder);
  for (int index = 0; index < 32; ++index) {
    std::ofstream(folder / ("file" + std::to_string(index)));
  }
  FILE* screen = tmpfile();
  fflush(stdout);
  const int saved_out = dup(STDOUT_FILENO), saved_in = dup(STDIN_FILENO);
  const int null = open("/dev/null", O_RDONLY);
  if (!screen || saved_out < 0 || saved_in < 0 || null < 0 ||
      dup2(fileno(screen), STDOUT_FILENO) < 0 || dup2(null, STDIN_FILENO) < 0) {
    perror("cannot redirect benchmark output");
    return;
  }
  close(null);
  const bool prior_tty = g_tty, prior_color = g_color;
  g_tty = g_color = true;
  setenv("COLUMNS", "100", 1);
  SetPersistentComposer(true);
  double milliseconds = 0;
  {
    InteractiveOutput output;
    LiveRegion region(output);
    TerminalInputDecoder keys;
    RawComposer composer(output, region, keys);
    composer.Mount(InputPrompt(), "look at @" + folder.string() + "/");
    MdStream markdown;
    output.Start();
    const auto started = std::chrono::steady_clock::now();
    size_t index = 0;
    milliseconds = Measure(kTokens, [&] {
      markdown.Feed(styled && index % 3 == 0
                        ? "`code" + std::to_string(index++) + "` "
                        : "word" + std::to_string(index++) + " ");
      fflush(stdout);
      auto update = output.Read();
      if (update.changed) {
        region.Commit(std::move(update.committed));
        region.SetTail(std::move(update.tail));
      }
      region.SetStatus(StatusBarLine(
          ActivityBar({.elapsed = std::chrono::steady_clock::now() - started,
                       .context_used = 12000,
                       .context_window = 200000,
                       .model = "benchmark-model"})));
      const RawComposer::Layout draft = composer.View();
      region.SetComposer(draft.rows, draft.caret_row, draft.caret_col);
      region.Flush();
      return size_t{1};
    });
    markdown.Flush();
    output.Stop();
  }
  SetPersistentComposer(false);
  unsetenv("COLUMNS");
  g_tty = prior_tty;
  g_color = prior_color;
  fflush(stdout);
  const off_t bytes = lseek(STDOUT_FILENO, 0, SEEK_END);
  dup2(saved_out, STDOUT_FILENO);
  dup2(saved_in, STDIN_FILENO);
  close(saved_out);
  close(saved_in);
  fclose(screen);
  fs::remove_all(folder);
  Report(styled ? "paint, code spans" : "paint, plain", kTokens, milliseconds);
  std::cout << "  " << bytes / static_cast<off_t>(kTokens)
            << " terminal bytes per token\n";
}

// A frame of the 1 MiB limit arriving in the reads ReadFrames makes.
void BenchmarkFrameBuffer() {
  const std::string frame =
      JsonDump(json{{"v", session::kProtocol},
                    {"text", std::string(session::kFrameBytes - 64, 'x')}}) +
      "\n";
  constexpr size_t kFrames = 20;
  Report("frame buffer, 1 MiB", kFrames, Measure(kFrames, [&] {
           session::FrameBuffer buffer;
           size_t received = 0;
           for (size_t at = 0; at < frame.size();
                at += session::kIoBufferBytes) {
             buffer.Feed(
                 std::string_view(frame).substr(at, session::kIoBufferBytes),
                 [&](const json&) { return ++received > 0; });
           }
           return received;
         }));
}

// A saved conversation of a long working session, as each model call, each
// save and each checkpoint handles it whole. Built unless `--session PATH`
// names a saved one: 250 tool rounds (a call and a result of about 1 KB, one
// round in eight 8 KB) between 30 user messages.
void BenchmarkLongConversation(std::string path) {
  char directory[] = "/tmp/uagent-bench-XXXXXX";
  if (!mkdtemp(directory)) {
    perror("cannot create benchmark directory");
    return;
  }
  Api api(RuntimeConfig{});
  std::vector<Tool> tools;
  ProcessSupervisor processes;
  UsageAccumulator usage;
  Agent agent(api, tools, processes, usage,
              [](const Tool&, const json&, int64_t) { return "denied"; });
  std::string error;
  if (path.empty()) {
    path = std::string(directory) + "/built.json";
    if (!agent.Save(path, error)) {
      std::cerr << "cannot save benchmark conversation: " << error << '\n';
      return;
    }
    std::ifstream saved(path);
    std::string header;
    std::getline(saved, header);
    json body = json::parse(saved, nullptr, false);
    for (size_t round = 0; round < 250; ++round) {
      if (round % 8 == 0) {
        body["messages"].push_back(
            {{"role", "user"}, {"content", std::string(400, 'u')}});
        body["message_kinds"].push_back("user");
      }
      const std::string id = "call-" + std::to_string(round);
      const size_t result_bytes = round % 8 == 7 ? KiB(8) : KiB(1);
      body["messages"].push_back(
          {{"role", "assistant"},
           {"content", std::string(200, 'a')},
           {"tool_calls",
            {{{"id", id},
              {"type", "function"},
              {"function",
               {{"name", "run"},
                {"arguments",
                 JsonDump({{"command", std::string(600, 'c')}})}}}}}}});
      body["message_kinds"].push_back("assistant");
      body["messages"].push_back({{"role", "tool"},
                                  {"tool_call_id", id},
                                  {"content", std::string(result_bytes, 'r')}});
      body["message_kinds"].push_back("tool_result");
    }
    body["display"] = json::object();
    std::ofstream(path) << header << '\n' << JsonDump(body);
  }
  std::ifstream saved(path);
  std::string header;
  std::getline(saved, header);
  if (!agent.Load(path,
                  JsonValue(json::parse(header, nullptr, false), "cwd", ""),
                  error)) {
    std::cerr << "cannot load " << path << ": " << error << '\n';
    return;
  }
  constexpr size_t kRepeats = 10;
  size_t request_bytes = 0, saved_bytes = 0, view_bytes = 0;
  Report("long conversation request", kRepeats, Measure(kRepeats, [&] {
           request_bytes = JsonDump(agent.ModelRequest()).size();
           return request_bytes;
         }));
  const std::string copy = std::string(directory) + "/saved.json";
  Report("long conversation save", kRepeats, Measure(kRepeats, [&] {
           agent.Save(copy, error);
           saved_bytes = std::filesystem::file_size(copy);
           return saved_bytes;
         }));
  Report("long conversation view", kRepeats, Measure(kRepeats, [&] {
           view_bytes = JsonDump(agent.DisplaySnapshot()).size();
           return view_bytes;
         }));
  std::cout << "long conversation            " << agent.MessageCount()
            << " messages; request " << request_bytes << " bytes, save "
            << saved_bytes << " bytes, view " << view_bytes << " bytes\n";
  std::filesystem::remove_all(directory);
}

}  // namespace

int RunBenchmarks(const std::string& session) {
  constexpr size_t kIterations = 10000;
  const std::string hostile = "normal text\n\x1b]52;c;payload\x07\tmore text\n";
  const std::string large(24000, 'x');

  bool prior_tty = g_tty;
  g_tty = true;
  Report("terminal sanitization", kIterations,
         Measure(kIterations, [&] { return TerminalSafe(hostile).size(); }));
  g_tty = prior_tty;

  Report("tool-result cap", kIterations, Measure(kIterations, [&] {
           return HeadTail(large, static_cast<size_t>(ToolResultCap())).size();
         }));

  BenchmarkStream();
  BenchmarkActivity(false);
  BenchmarkActivity(true);
  BenchmarkStreamedAnswer();
  BenchmarkCommandOutput();
  BenchmarkCheckpointMerge();
  BenchmarkReplayRead();
  BenchmarkAttachedDocument();
  BenchmarkTerminalPaint(false);
  BenchmarkTerminalPaint(true);
  BenchmarkFrameBuffer();
  BenchmarkLongConversation(session);

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
    cached.wire_api = wire;
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

// `uagent_bench [--session PATH]`: PATH is a saved conversation to measure
// instead of the built one.
int main(int argc, char** argv) {
  const bool session = argc == 3 && std::string(argv[1]) == "--session";
  return uagent::RunBenchmarks(session ? argv[2] : "");
}
