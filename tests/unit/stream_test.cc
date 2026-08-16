// Copyright 2026 Timon Gentzsch

#include "include/api/stream.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/api/retry.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestSseChunkPartitions() {
  auto event = [](json value, const char* ending = "\n\n") {
    return "data: " + value.dump() + ending;
  };

  std::string wire = ": keepalive\r\n\r\n";
  wire += "event: message\n";
  wire += "data: malformed-json\n\n";
  wire += event({{"usage", {{"prompt_tokens", 7}, {"completion_tokens", 3}}}});
  wire += event(
      {{"choices",
        {{{"delta",
           {{"reasoning", "think "},
            {"reasoning_content", "duplicate alias"},
            {"reasoning_details", json::array({{{"type", "reasoning.text"},
                                                {"format", "unknown"},
                                                {"index", 0},
                                                {"text", "think "}}})}}}}}}});
  wire += event({{"choices",
                  {{{"delta",
                     {{"reasoning_content", "carefully"},
                      {"reasoning_details",
                       json::array({{{"type", "reasoning.text"},
                                     {"format", "unknown"},
                                     {"index", 0},
                                     {"text", "carefully"}}})}}}}}}});
  wire += event({{"choices", {{{"delta", {{"content", "Hel"}}}}}}}, "\r\n\r\n");
  wire += event({{"choices", {{{"delta", {{"content", "lo"}}}}}}});
  wire += event({{"choices",
                  {{{"delta",
                     {{"tool_calls",
                       json::array({{{"index", 0},
                                     {"id", "call_"},
                                     {"function",
                                      {{"name", "read_"},
                                       {"arguments", "{\"path\":"}}}}})}}}}}}});
  wire += event(
      {{"choices",
        {{{"delta",
           {{"reasoning", "after"},
            {"reasoning_details", json::array({{{"type", "reasoning.text"},
                                                {"format", "unknown"},
                                                {"index", 0},
                                                {"text", "after"}}})}}}}}}});
  wire += event(
      {{"choices",
        {{{"delta",
           {{"tool_calls", json::array({{{"index", 0},
                                         {"id", "1"},
                                         {"function",
                                          {{"name", "file"},
                                           {"arguments", "\"x\"}"}}}}})}}}}}}});
  wire += event(
      {{"choices",
        {{{"delta",
           {{"tool_calls",
             json::array({{{"index", 0},
                           {"id", "call_1"},
                           {"function", {{"name", "read_file"}}}}})}}}}}}});
  wire +=
      event({{"choices",
              {{{"delta",
                 {{"annotations",
                   json::array({{{"type", "url_citation"},
                                 {"url_citation",
                                  {{"url", "https://example.com"}}}}})}}}}}}});
  wire += event({{"choices", {{{"finish_reason", "tool_calls"}}}}});
  wire += "data: [DONE]\n\n";

  struct Parsed {
    ChatResult result;
    std::map<int, ToolCall> calls;
  };
  auto parse = [&](size_t split, bool bytewise = false) {
    Parsed parsed;
    StreamCtx stream;
    stream.res = &parsed.result;
    stream.status = 200;
    stream.render_output = false;
    stream.started = std::chrono::steady_clock::now();
    if (bytewise) {
      for (char byte : wire) CHECK(stream.Feed(&byte, 1) == 1);
    } else {
      CHECK(stream.Feed(wire.data(), split) == split);
      CHECK(stream.Feed(wire.data() + split, wire.size() - split) ==
            wire.size() - split);
    }
    stream.Finish();
    parsed.calls = std::move(stream.calls);
    return parsed;
  };
  auto verify_parsed = [&](const Parsed& parsed) {
    CHECK(parsed.result.content == "Hello");
    CHECK(parsed.result.reasoning == "think carefullyafter");
    CHECK(parsed.result.reasoning_content_field);
    CHECK(parsed.result.reasoning_details.size() == 1);
    if (parsed.result.reasoning_details.size() == 1) {
      CHECK(parsed.result.reasoning_details[0]["text"] ==
            "think carefullyafter");
    }
    CHECK(parsed.result.error.empty());
    CHECK(parsed.result.finish_reason == "tool_calls");
    CHECK(parsed.result.first_event_ms >= 0);
    CHECK(parsed.result.usage["prompt_tokens"] == 7);
    CHECK(parsed.result.usage["completion_tokens"] == 3);
    CHECK(parsed.result.annotations.size() == 1);
    CHECK(parsed.calls.size() == 1);
    if (parsed.calls.size() == 1) {
      const ToolCall& call = parsed.calls.begin()->second;
      CHECK(call.id == "call_1");
      CHECK(call.name == "read_file");
      CHECK(call.args == R"({"path":"x"})");
    }
  };

  verify_parsed(parse(wire.size()));
  verify_parsed(parse(0, true));
  for (size_t split = 0; split <= wire.size(); ++split) {
    verify_parsed(parse(split));
  }

  ChatResult unindexed;
  std::map<int, ToolCall> no_calls;
  auto reasoning_event = [](const char* text, const char* data) {
    return JsonDump(
        {{"choices",
          {{{"delta",
             {{"reasoning_details",
               json::array(
                   {{{"type", "reasoning.text"}, {"text", text}},
                    {{"type", "reasoning.encrypted"}, {"data", data}}})}}}}}}});
  };
  OpenAiStreamDelta first_details =
      DecodeOpenAiStreamEvent(reasoning_event("one", "a"), unindexed, no_calls);
  OpenAiStreamDelta second_details = DecodeOpenAiStreamEvent(
      reasoning_event(" two", "b"), unindexed, no_calls);
  CHECK(first_details.reasoning == "one");
  CHECK(second_details.reasoning == " two");
  CHECK(unindexed.reasoning_details.size() == 2);
  CHECK(unindexed.reasoning_details[0]["text"] == "one two");
  CHECK(unindexed.reasoning_details[1]["data"] == "ab");

  OpenAiStreamDelta summary_delta = DecodeOpenAiStreamEvent(
      R"({"choices":[{"delta":{"reasoning_details":[{"type":"reasoning.summary","summary":"Summary chunk"}]}}]})",
      unindexed, no_calls);
  CHECK(summary_delta.reasoning == "Summary chunk");
  CHECK(unindexed.reasoning_details.size() == 3);

  ChatResult aliased_reasoning;
  OpenAiStreamDelta aliased_delta = DecodeOpenAiStreamEvent(
      R"({"choices":[{"delta":{"reasoning":"once","reasoning_details":[{"type":"reasoning.text","text":"once"}]}}]})",
      aliased_reasoning, no_calls);
  CHECK(aliased_delta.reasoning == "once");
  CHECK(aliased_reasoning.reasoning_details_field);

  ChatResult empty_details;
  DecodeOpenAiStreamEvent(R"({"choices":[{"delta":{"reasoning_details":[]}}]})",
                          empty_details, no_calls);
  CHECK(empty_details.reasoning_details_field);
  CHECK(empty_details.reasoning_details.empty());

  ChatResult empty_alias;
  OpenAiStreamDelta details_fallback = DecodeOpenAiStreamEvent(
      R"({"choices":[{"delta":{"reasoning":"","reasoning_details":[{"type":"reasoning.text","text":"details only"}]}}]})",
      empty_alias, no_calls);
  CHECK(details_fallback.reasoning == "details only");

  CHECK(ReasoningStatusPreview("first line\nlatest line\n") == "latest line");
  CHECK(ReasoningStatusPreview("  padded latest  ") == "padded latest");
  std::string long_preview = ReasoningStatusPreview(std::string(140, 'x'));
  CHECK(long_preview.starts_with("…"));
  CHECK(long_preview.size() <= 123);

  std::string final_line =
      event({{"choices", {{{"delta", {{"content", "complete"}}}}}}});
  final_line.resize(final_line.size() - 2);
  ChatResult result;
  StreamCtx stream;
  stream.res = &result;
  stream.status = 200;
  stream.render_output = false;
  stream.started = std::chrono::steady_clock::now();
  CHECK(stream.Feed(final_line.data(), final_line.size()) == final_line.size());
  CHECK(result.content.empty());
  stream.Finish();
  CHECK(result.content == "complete");

  StreamCtx compact;
  ChatResult compact_result;
  compact.res = &compact_result;
  compact.render_output = true;
  compact.full_reasoning = false;
  compact.OutputReasoning("checking include dependencies");
  CHECK(!compact.in_reasoning);
  CHECK(compact.reasoning_preview == "checking include dependencies");
  StreamCtx full_reasoning;
  ChatResult full_result;
  full_reasoning.res = &full_result;
  full_reasoning.render_output = true;
  full_reasoning.OutputReasoning("full reasoning");
  CHECK(full_reasoning.in_reasoning);
  uint64_t activity = BeginTerminalActivity("working");
  UpdateTerminalActivity(activity, "thinking · compact preview");
  CHECK(CurrentTerminalActivity() == "thinking · compact preview");
  EndTerminalActivity(activity);

  std::map<int, ToolCall> missing_id = {
      {0, ToolCall{"", "read_file", R"({"path":"x"})"}}};
  ChatResult normalized;
  CHECK(CollectToolCalls(missing_id, normalized));
  CHECK(normalized.tool_calls.size() == 1);
  CHECK(normalized.tool_calls[0].id == "uagent-call-0");

  std::map<int, ToolCall> duplicate_ids = {
      {0, ToolCall{"same", "read_file", R"({"path":"x"})"}},
      {1, ToolCall{"same", "list_dir", R"({"path":"."})"}}};
  normalized = {};
  CHECK(CollectToolCalls(duplicate_ids, normalized));
  CHECK(normalized.tool_calls.size() == 2);
  CHECK(normalized.tool_calls[0].id == "same");
  CHECK(normalized.tool_calls[1].id == "same-2");

  std::map<int, ToolCall> malformed = {
      {0, ToolCall{"call", "read_file", R"({"path")"}}};
  ChatResult invalid;
  CHECK(!CollectToolCalls(malformed, invalid));
  CHECK(invalid.error.find("incomplete function") != std::string::npos);
  CHECK(invalid.tool_calls.empty());

  ChatResult usage_then_error;
  std::map<int, ToolCall> no_tool_calls;
  DecodeOpenAiStreamEvent(
      R"({"usage":{"prompt_tokens":1,"completion_tokens":0}})",
      usage_then_error, no_tool_calls);
  CHECK(usage_then_error.semantic_progress);
  DecodeOpenAiStreamEvent(
      R"({"error":{"type":"server_error","code":"server_error","message":"retry"}})",
      usage_then_error, no_tool_calls);
  CHECK(!SafeToRetry(usage_then_error));
}

void TestSseFraming() {
  const std::string wire =
      ": keepalive\r\n"
      "id: 7\r\n"
      "event: update\r\n"
      "data: first\r\n"
      "data: second\r\n\r\n"
      "data:\n\n";
  for (size_t split = 0; split <= wire.size(); ++split) {
    SseParser parser;
    CHECK(parser.Feed(std::string_view(wire).substr(0, split)));
    CHECK(parser.Feed(std::string_view(wire).substr(split)));
    CHECK(parser.Finish());
    std::vector<SseEvent> events = parser.TakeEvents();
    CHECK(events.size() == 2);
    if (events.size() == 2) {
      CHECK(events[0].event == "update");
      CHECK(events[0].data == "first\nsecond");
      CHECK(events[0].id == "7");
      CHECK(events[1].event == "message");
      CHECK(events[1].data.empty());
      CHECK(events[1].id == "7");
    }
  }

  SseParser final_event;
  CHECK(final_event.Feed("data: tail"));
  CHECK(final_event.TakeEvents().empty());
  CHECK(final_event.Finish());
  CHECK(final_event.TakeEvents()[0].data == "tail");

  SseParser bounded(8);
  CHECK(!bounded.Feed("data: payload-too-large\n\n"));
  CHECK(!bounded.Error().empty());
}

void TestBackgroundValidation() {
  namespace fs = std::filesystem;
  ProcessSupervisor supervisor;
  auto add_job = [&](BgJob job) {
    CHECK(supervisor.TryAdd(std::move(job), 100));
  };
  add_job({999998, "", "", true, ""});
  CHECK(!supervisor.PendingCount());
  CHECK(supervisor.DetachedCount() == 1);
  add_job({999997, "", "", false, ""});
  CHECK(supervisor.PendingCount());
  CHECK(supervisor.PendingCount() == 1);
  CHECK(supervisor.Count() == 2);
  CHECK(!supervisor.Find(999997).has_value());
  CHECK(supervisor.Find(ActivityId(supervisor.Snapshot().back())).has_value());
  CHECK(!supervisor.TryAdd({999996, "", "", false, ""}, 1));
  CHECK(supervisor.Snapshot().size() == supervisor.Count());
  CHECK(!supervisor.TakeAllForShutdown().empty());
  std::vector<Tool> tools = BuiltinTools(supervisor);
  CHECK(FindTool(tools, "wait_background") == nullptr);
  const Tool* edit = FindTool(tools, "edit_file");
  CHECK(edit && edit->parameters["properties"].contains("edits"));
  CHECK(edit && !edit->parameters["properties"].contains("old"));
  if (edit) {
    CHECK(ToolSummary(*edit, {{"path", "x"},
                              {"edits",
                               json::array({{{"old", "a"}, {"new", "b"}}})}}) ==
          "x (1 edit)");
    CHECK(ToolSummary(*edit, {{"path", "x"},
                              {"edits",
                               json::array({{{"old", "a"}, {"new", "b"}},
                                            {{"old", "c"}, {"new", "d"}}})}}) ==
          "x (2 edits)");
  }
  // read-only and independent-process tools must be able to overlap, and the
  // schema has to say so or the model has no reason to batch them
  for (const char* name :
       {"read_file", "list_dir", "grep", "run", "activity_output"}) {
    const Tool* tool = FindTool(tools, name);
    CHECK(tool && tool->parallel_safe);
    if (tool) {
      CHECK(ToolDescription(*tool).find("Batchable") != std::string::npos);
    }
  }

  fs::path log =
      fs::temp_directory_path() /
      ("uagent-utf8-tail-" + std::to_string(static_cast<int64_t>(getpid())));
  CHECK(ToolWritePrivateFile(log.string(), "abc😀tail")
            .output.starts_with("wrote "));
  std::string tail = ReadLogTail(log.string(), 7);  // begins inside 😀
  CHECK(JsonDump(json(tail)).find("\x9F") == std::string::npos);
  std::error_code ec;
  fs::remove(log, ec);
}

}  // namespace uagent
