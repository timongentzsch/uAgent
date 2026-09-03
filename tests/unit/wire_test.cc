// Copyright 2026 Timon Gentzsch

#include "include/api/wire.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/api/stream.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

json FunctionSchema(const std::string& name) {
  return {{"type", "function"},
          {"function",
           {{"name", name},
            {"description", "test tool"},
            {"parameters",
             {{"type", "object"},
              {"properties", {{"path", {{"type", "string"}}}}},
              {"required", json::array({"path"})}}}}}};
}

size_t ItemsOfType(const json& values, std::string_view type) {
  if (!values.is_array()) return 0;
  size_t count = 0;
  for (const json& value : values) {
    if (JsonValue(value, "type", "") == type) ++count;
  }
  return count;
}

}  // namespace

void TestWireAdapters() {
  json schemas =
      json::array({FunctionSchema("web_search"), FunctionSchema("read_path")});
  json messages = json::array(
      {{{"role", "system"}, {"content", "baseline"}},
       {{"role", "user"},
        {"content",
         json::array(
             {{{"type", "text"}, {"text", "inspect"}},
              {{"type", "image_url"},
               {"image_url",
                {{"url", "data:image/png;base64,AA=="}, {"detail", "low"}}}},
              {{"type", "file"},
               {"file",
                {{"filename", "note.pdf"},
                 {"file_data", "data:application/pdf;base64,JVBERg=="}}}}})}},
       {{"role", "assistant"},
        {"content", "calling"},
        {"tool_calls",
         json::array({{{"id", "call-1"},
                       {"type", "function"},
                       {"function",
                        {{"name", "read_path"},
                         {"arguments", R"({"path":"README.md"})"}}}}})},
        {kWireReplayField,
         {{"wire_api", "responses"},
          {"items", json::array({{{"type", "reasoning"},
                                  {"encrypted_content", "opaque"}}})}}}},
       {{"role", "tool"},
        {"tool_call_id", "call-1"},
        {"content", "contents"}}});

  WireRequest responses_request{"gpt-test", messages, schemas, "high", 4096,
                                true,       true,     false,   true,   true};
  json responses = EncodeWireRequest(WireApi::kResponses, responses_request);
  CHECK(responses["model"] == "gpt-test");
  CHECK(responses["store"] == false);
  CHECK(responses["max_output_tokens"] == 4096);
  CHECK(responses["reasoning"]["effort"] == "high");
  CHECK(ItemsOfType(responses["input"], "reasoning") == 1);
  CHECK(ItemsOfType(responses["input"], "function_call") == 1);
  CHECK(ItemsOfType(responses["input"], "function_call_output") == 1);
  CHECK(ItemsOfType(responses["tools"], "web_search") == 1);
  CHECK(ItemsOfType(responses["tools"], "function") == 1);
  CHECK(responses["tools"][0]["name"] == "read_path");
  CHECK(responses["parallel_tool_calls"] == true);
  CHECK(responses["input"][1]["content"][1]["type"] == "input_image");
  CHECK(responses["input"][1]["content"][2]["type"] == "input_file");
  CHECK(WireEndpoint(WireApi::kResponses) == "/responses");

  json anthropic_messages = messages;
  anthropic_messages[2][kWireReplayField] = {
      {"wire_api", "anthropic_messages"},
      {"content", json::array({{{"type", "thinking"},
                                {"thinking", "consider"},
                                {"signature", "signed"}},
                               {{"type", "tool_use"},
                                {"id", "call-1"},
                                {"name", "read_path"},
                                {"input", {{"path", "README.md"}}}}})}};
  WireRequest anthropic_request{"claude-test", anthropic_messages,
                                schemas,       "high",
                                2048,          true,
                                true,          false,
                                true,          true};
  json anthropic =
      EncodeWireRequest(WireApi::kAnthropicMessages, anthropic_request);
  CHECK(anthropic["system"] == "baseline");
  CHECK(anthropic["max_tokens"] == 2048);
  CHECK(anthropic["cache_control"]["type"] == "ephemeral");
  CHECK(anthropic["thinking"]["type"] == "adaptive");
  CHECK(anthropic["output_config"]["effort"] == "high");
  CHECK(anthropic["messages"][0]["content"][1]["type"] == "image");
  CHECK(anthropic["messages"][0]["content"][2]["type"] == "document");
  CHECK(anthropic["messages"][1]["content"].size() == 2);
  CHECK(anthropic["messages"][1]["content"][0]["signature"] == "signed");
  CHECK(anthropic["messages"][2]["content"][0]["type"] == "tool_result");
  CHECK(ItemsOfType(anthropic["tools"], "web_search_20250305") == 1);
  CHECK(anthropic["tools"].size() == 2);
  CHECK(WireEndpoint(WireApi::kAnthropicMessages) == "/messages");

  WireRequest chat_request{"chat-test", messages, schemas, "",   -1,
                           true,        true,     true,    true, true};
  json chat = EncodeWireRequest(WireApi::kChatCompletions, chat_request);
  CHECK(chat.contains("messages"));
  CHECK(!chat["messages"][2].contains(kWireReplayField));
  CHECK(chat["tools"].size() == 2);
  CHECK(chat["stream_options"]["include_usage"] == true);
  CHECK(WireEndpoint(WireApi::kChatCompletions) == "/chat/completions");
  Api switched(RuntimeConfig{});
  switched.model = "chat-test";
  switched.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, "https://chat.test/v1");
  json switched_payload =
      json::parse(switched.ChatPayload(messages, schemas), nullptr, false);
  CHECK(switched_payload.is_object());
  CHECK(!switched_payload["messages"][2].contains(kWireReplayField));

  RuntimeConfig config;
  config.web_search_backend = "auto";
  Api api(config);
  api.model = "gpt-name-does-not-imply-search";
  api.capabilities = CapabilitiesForRoute(ProviderProtocol::kOpenAi,
                                          "https://provider.test/v1",
                                          WireApi::kResponses, false);
  CHECK(!api.NativeHostedTool(HostedTool::kWebSearch));
  json fallback = api.BuildRequestBody(json::array(), schemas);
  CHECK(ItemsOfType(fallback["tools"], "web_search") == 0);
  CHECK(ItemsOfType(fallback["tools"], "function") == 2);

  api.capabilities = CapabilitiesForRoute(ProviderProtocol::kOpenAi,
                                          "https://provider.test/v1",
                                          WireApi::kResponses, true);
  CHECK(api.NativeHostedTool(HostedTool::kWebSearch));
  bool web_available = false;
  json native =
      api.BuildRequestBody(json::array(), schemas, "", &web_available);
  CHECK(web_available);
  CHECK(ItemsOfType(native["tools"], "web_search") == 1);
  CHECK(ItemsOfType(native["tools"], "function") == 1);

  api.config.web_search_backend = "openrouter";
  CHECK(!api.NativeHostedTool(HostedTool::kWebSearch));
  json separate = api.BuildRequestBody(json::array(), schemas);
  CHECK(ItemsOfType(separate["tools"], "web_search") == 0);
  CHECK(ItemsOfType(separate["tools"], "function") == 2);

  api.config.web_search_backend = "off";
  web_available = true;
  json disabled =
      api.BuildRequestBody(json::array(), schemas, "", &web_available);
  CHECK(!web_available);
  CHECK(ItemsOfType(disabled["tools"], "web_search") == 0);
  CHECK(ItemsOfType(disabled["tools"], "function") == 1);
  CHECK(disabled["tools"][0]["name"] == "read_path");

  CHECK(WireSupportsHostedTool(WireApi::kResponses, HostedTool::kWebSearch));
  CHECK(WireSupportsHostedTool(WireApi::kAnthropicMessages,
                               HostedTool::kWebSearch));
  CHECK(!WireSupportsHostedTool(WireApi::kChatCompletions,
                                HostedTool::kWebSearch));
}

void TestWireStreams() {
  ChatResult responses_result;
  std::map<int, ToolCall> responses_calls;
  WireStreamState responses_state;
  std::string answer;
  std::string reasoning;
  auto responses_event = [&](const json& value) {
    WireStreamDelta delta = DecodeWireStreamEvent(
        WireApi::kResponses, JsonDump(value), responses_result, responses_calls,
        responses_state);
    answer += delta.content;
    reasoning += delta.reasoning;
  };
  responses_event(
      {{"type", "response.reasoning_text.delta"}, {"delta", "think"}});
  responses_event(
      {{"type", "response.output_item.done"},
       {"output_index", 0},
       {"item", {{"type", "reasoning"}, {"encrypted_content", "encrypted"}}}});
  responses_event({{"type", "response.output_item.done"},
                   {"output_index", 1},
                   {"item",
                    {{"type", "web_search_call"},
                     {"id", "search-1"},
                     {"status", "completed"}}}});
  responses_event({{"type", "response.output_item.added"},
                   {"output_index", 2},
                   {"item",
                    {{"type", "function_call"},
                     {"call_id", "call-1"},
                     {"name", "read_path"},
                     {"arguments", ""}}}});
  responses_event({{"type", "response.function_call_arguments.delta"},
                   {"output_index", 2},
                   {"call_id", "call-1"},
                   {"delta", R"({"path":)"}});
  responses_event({{"type", "response.function_call_arguments.done"},
                   {"output_index", 2},
                   {"call_id", "call-1"},
                   {"arguments", R"({"path":"README.md"})"}});
  responses_event({{"type", "response.output_item.done"},
                   {"output_index", 2},
                   {"item",
                    {{"type", "function_call"},
                     {"call_id", "call-1"},
                     {"name", "read_path"},
                     {"arguments", R"({"path":"README.md"})"}}}});
  responses_event(
      {{"type", "response.output_text.delta"}, {"delta", "answer"}});
  responses_event({{"type", "response.output_text.annotation.added"},
                   {"annotation",
                    {{"type", "url_citation"},
                     {"url", "https://example.test/source"},
                     {"title", "Source"}}}});
  responses_event(
      {{"type", "response.completed"},
       {"response",
        {{"status", "completed"},
         {"usage", {{"input_tokens", 10}, {"output_tokens", 4}}}}}});
  CHECK(answer == "answer");
  CHECK(reasoning == "think");
  CHECK(responses_result.reasoning_field);
  CHECK(responses_result.finish_reason == "completed");
  CHECK(responses_result.annotations.size() == 1);
  CHECK(responses_result.replay["items"].size() == 3);
  CHECK(responses_result
            .usage["server_tool_use_details"]["web_search_requests"] == 1);
  CHECK(CollectToolCalls(responses_calls, responses_result));
  CHECK(responses_result.tool_calls.size() == 1);
  CHECK(responses_result.tool_calls[0].name == "read_path");
  CHECK(responses_result.tool_calls[0].args == R"({"path":"README.md"})");

  // An incomplete Responses turn is a typed stop, not a transport failure.
  // Complete calls remain executable so an output-token cutoff does not
  // repeat an already formed side effect.
  ChatResult incomplete_result;
  std::map<int, ToolCall> incomplete_calls;
  WireStreamState incomplete_state;
  auto incomplete_event = [&](const json& value) {
    return DecodeWireStreamEvent(WireApi::kResponses, JsonDump(value),
                                 incomplete_result, incomplete_calls,
                                 incomplete_state);
  };
  incomplete_event({{"type", "response.output_item.done"},
                    {"output_index", 0},
                    {"item",
                     {{"type", "function_call"},
                      {"call_id", "cutoff-call"},
                      {"name", "read_path"},
                      {"arguments", R"({"path":"README.md"})"}}}});
  incomplete_event(
      {{"type", "response.incomplete"},
       {"response",
        {{"incomplete_details", {{"reason", "max_output_tokens"}}},
         {"usage", {{"input_tokens", 4}, {"output_tokens", 2}}}}}});
  CHECK(incomplete_result.error.empty());
  CHECK(incomplete_result.incomplete);
  CHECK(incomplete_result.stop_cause == ResponseStopCause::kLength);
  CHECK(incomplete_result.stop_details["reason"] == "max_output_tokens");
  CHECK(CollectToolCalls(incomplete_calls, incomplete_result));
  CHECK(incomplete_result.tool_calls.size() == 1);
  CHECK(incomplete_result.tool_calls[0].id == "cutoff-call");

  ChatResult anthropic_result;
  std::map<int, ToolCall> anthropic_calls;
  WireStreamState anthropic_state;
  std::string anthropic_answer;
  std::string anthropic_reasoning;
  auto anthropic_event = [&](const json& value) {
    WireStreamDelta delta = DecodeWireStreamEvent(
        WireApi::kAnthropicMessages, JsonDump(value), anthropic_result,
        anthropic_calls, anthropic_state);
    anthropic_answer += delta.content;
    anthropic_reasoning += delta.reasoning;
  };
  anthropic_event(
      {{"type", "message_start"},
       {"message",
        {{"usage", {{"input_tokens", 7}, {"cache_read_input_tokens", 3}}}}}});
  anthropic_event(
      {{"type", "content_block_start"},
       {"index", 0},
       {"content_block", {{"type", "thinking"}, {"thinking", ""}}}});
  anthropic_event(
      {{"type", "content_block_delta"},
       {"index", 0},
       {"delta", {{"type", "thinking_delta"}, {"thinking", "consider"}}}});
  anthropic_event(
      {{"type", "content_block_delta"},
       {"index", 0},
       {"delta", {{"type", "signature_delta"}, {"signature", "signed"}}}});
  anthropic_event({{"type", "content_block_start"},
                   {"index", 1},
                   {"content_block",
                    {{"type", "server_tool_use"},
                     {"id", "search-call"},
                     {"name", "web_search"},
                     {"input", {{"query", "C++20"}}}}}});
  anthropic_event(
      {{"type", "content_block_start"},
       {"index", 2},
       {"content_block",
        {{"type", "web_search_tool_result"},
         {"tool_use_id", "search-call"},
         {"content", json::array({{{"type", "web_search_result"},
                                   {"url", "https://example.test/cpp"},
                                   {"title", "C++"},
                                   {"encrypted_content", "opaque"}}})}}}});
  anthropic_event({{"type", "content_block_start"},
                   {"index", 3},
                   {"content_block", {{"type", "text"}, {"text", ""}}}});
  anthropic_event({{"type", "content_block_delta"},
                   {"index", 3},
                   {"delta",
                    {{"type", "citations_delta"},
                     {"citation",
                      {{"type", "web_search_result_location"},
                       {"url", "https://example.test/cpp"},
                       {"title", "C++"},
                       {"cited_text", "source text"}}}}}});
  anthropic_event({{"type", "content_block_delta"},
                   {"index", 3},
                   {"delta", {{"type", "text_delta"}, {"text", "result"}}}});
  anthropic_event({{"type", "content_block_start"},
                   {"index", 4},
                   {"content_block",
                    {{"type", "tool_use"},
                     {"id", "tool-1"},
                     {"name", "read_path"},
                     {"input", json::object()}}}});
  anthropic_event(
      {{"type", "content_block_delta"},
       {"index", 4},
       {"delta",
        {{"type", "input_json_delta"}, {"partial_json", R"({"path":"x"})"}}}});
  anthropic_event({{"type", "content_block_stop"}, {"index", 4}});
  anthropic_event({{"type", "message_delta"},
                   {"delta", {{"stop_reason", "pause_turn"}}},
                   {"usage", {{"output_tokens", 5}}}});
  anthropic_event({{"type", "message_stop"}});
  CHECK(anthropic_answer == "result");
  CHECK(anthropic_reasoning == "consider");
  CHECK(anthropic_result.stop_cause == ResponseStopCause::kPause);
  CHECK(anthropic_result.finish_reason == "pause_turn");
  CHECK(anthropic_result.annotations.size() == 1);
  CHECK(anthropic_result.replay["content"].size() == 5);
  CHECK(anthropic_result.replay["content"][0]["signature"] == "signed");
  CHECK(anthropic_result.replay["content"][2]["content"][0]
                               ["encrypted_content"] == "opaque");
  CHECK(anthropic_result.usage["server_tool_use"]["web_search_requests"] == 1);
  CHECK(CollectToolCalls(anthropic_calls, anthropic_result));
  CHECK(anthropic_result.tool_calls.size() == 1);
  CHECK(anthropic_result.tool_calls[0].args == R"({"path":"x"})");

  // Chunk-boundary equivalence: the decoders are always fed by SseParser in
  // production, so the same bytes framed differently must decode identically.
  auto decode_split = [](WireApi api, const std::string& wire, size_t split) {
    SseParser parser;
    CHECK(parser.Feed(std::string_view(wire).substr(0, split)));
    CHECK(parser.Feed(std::string_view(wire).substr(split)));
    CHECK(parser.Finish());
    ChatResult result;
    std::map<int, ToolCall> calls;
    WireStreamState state;
    std::string text;
    for (const SseEvent& event : parser.TakeEvents()) {
      text +=
          DecodeWireStreamEvent(api, event.data, result, calls, state).content;
    }
    return text;
  };

  const std::string responses_wire =
      "event: response.output_text.delta\n"
      R"(data: {"type":"response.output_text.delta","delta":"al"})"
      "\n\n"
      R"(data: {"type":"response.output_text.delta","delta":"pha"})"
      "\n\n";
  for (size_t split = 0; split <= responses_wire.size(); ++split) {
    CHECK(decode_split(WireApi::kResponses, responses_wire, split) == "alpha");
  }

  const std::string anthropic_wire =
      R"(data: {"type":"content_block_start","index":0,)"
      R"("content_block":{"type":"text","text":""}})"
      "\n\n"
      R"(data: {"type":"content_block_delta","index":0,)"
      R"("delta":{"type":"text_delta","text":"be"}})"
      "\n\n"
      R"(data: {"type":"content_block_delta","index":0,)"
      R"("delta":{"type":"text_delta","text":"ta"}})"
      "\n\n";
  for (size_t split = 0; split <= anthropic_wire.size(); ++split) {
    CHECK(decode_split(WireApi::kAnthropicMessages, anthropic_wire, split) ==
          "beta");
  }
}

// A search the provider runs is visible progress, not a round this agent owes
// a result for. Both halves matter: the lifecycle has to reach a presenter,
// and the search must never reach the tool loop.
void TestWireStreamHostedSearch() {
  ChatResult result;
  std::map<int, ToolCall> calls;
  WireStreamState state;
  std::vector<std::pair<std::string, std::string>> steps;
  auto responses_event = [&](const json& value) {
    WireStreamDelta delta = DecodeWireStreamEvent(
        WireApi::kResponses, JsonDump(value), result, calls, state);
    CHECK(delta.activity);
    if (delta.hosted_tool) {
      json payload = HostedToolJson(*delta.hosted_tool);
      CHECK(payload["tool"] == "web_search");
      steps.emplace_back(payload["id"], payload["phase"]);
    }
  };
  responses_event({{"type", "response.output_item.added"},
                   {"output_index", 0},
                   {"item",
                    {{"type", "web_search_call"},
                     {"id", "ws_1"},
                     {"status", "in_progress"}}}});
  responses_event({{"type", "response.web_search_call.in_progress"},
                   {"output_index", 0},
                   {"item_id", "ws_1"}});
  responses_event({{"type", "response.web_search_call.searching"},
                   {"output_index", 0},
                   {"item_id", "ws_1"}});
  responses_event({{"type", "response.web_search_call.completed"},
                   {"output_index", 0},
                   {"item_id", "ws_1"}});
  // Responses reports the same finished search a second time as an output
  // item; a duplicate step would restart a spinner the row already left.
  responses_event(
      {{"type", "response.output_item.done"},
       {"output_index", 0},
       {"item",
        {{"type", "web_search_call"},
         {"id", "ws_1"},
         {"status", "completed"},
         {"action", {{"sources", json::array({{{"url", "https://a.test"}},
                                              {{"url", "https://b.test"}}})}}}}}});
  responses_event({{"type", "response.completed"},
                   {"response", {{"status", "completed"}, {"usage", json::object()}}}});
  const decltype(steps) expected_steps = {
      {"ws_1", "started"}, {"ws_1", "searching"}, {"ws_1", "completed"}};
  CHECK(steps == expected_steps);
  CHECK(result.usage["server_tool_use_details"]["web_search_requests"] == 1);
  CHECK(CollectToolCalls(calls, result));
  CHECK(result.tool_calls.empty());

  // A second search is its own lifecycle, not a continuation of the first.
  responses_event({{"type", "response.output_item.added"},
                   {"output_index", 1},
                   {"item", {{"type", "web_search_call"}, {"id", "ws_2"}}}});
  CHECK(steps.size() == 4);
  CHECK(steps.back() == std::make_pair(std::string("ws_2"),
                                       std::string("started")));

  // A search item the provider marks failed is a failure, not an empty result.
  ChatResult failed_result;
  std::map<int, ToolCall> failed_calls;
  WireStreamState failed_state;
  WireStreamDelta failed = DecodeWireStreamEvent(
      WireApi::kResponses,
      JsonDump(json{{"type", "response.output_item.done"},
                    {"output_index", 0},
                    {"item",
                     {{"type", "web_search_call"},
                      {"id", "ws_9"},
                      {"status", "failed"}}}}),
      failed_result, failed_calls, failed_state);
  REQUIRE(failed.hosted_tool.has_value());
  CHECK(failed.hosted_tool->phase == HostedToolPhase::kFailed);

  ChatResult anthropic_result;
  std::map<int, ToolCall> anthropic_calls;
  WireStreamState anthropic_state;
  std::vector<std::pair<std::string, std::string>> anthropic_steps;
  auto anthropic_event = [&](const json& value) {
    WireStreamDelta delta =
        DecodeWireStreamEvent(WireApi::kAnthropicMessages, JsonDump(value),
                              anthropic_result, anthropic_calls,
                              anthropic_state);
    if (!delta.hosted_tool) return;
    json payload = HostedToolJson(*delta.hosted_tool);
    anthropic_steps.emplace_back(payload["id"], payload["phase"]);
  };
  anthropic_event({{"type", "content_block_start"},
                   {"index", 0},
                   {"content_block",
                    {{"type", "server_tool_use"},
                     {"id", "srvtoolu_1"},
                     {"name", "web_search"},
                     {"input", {{"query", "C++26"}}}}}});
  anthropic_event(
      {{"type", "content_block_start"},
       {"index", 1},
       {"content_block",
        {{"type", "web_search_tool_result"},
         {"tool_use_id", "srvtoolu_1"},
         {"content", json::array({{{"type", "web_search_result"},
                                   {"url", "https://example.test/cpp"}}})}}}});
  anthropic_event({{"type", "content_block_start"},
                   {"index", 2},
                   {"content_block",
                    {{"type", "tool_use"},
                     {"id", "tool-1"},
                     {"name", "read_path"},
                     {"input", json::object()}}}});
  anthropic_event({{"type", "content_block_stop"}, {"index", 2}});
  anthropic_event({{"type", "message_stop"}});
  const decltype(anthropic_steps) expected_anthropic = {
      {"srvtoolu_1", "searching"}, {"srvtoolu_1", "completed"}};
  CHECK(anthropic_steps == expected_anthropic);
  // One search, counted once, though it is named by both of its blocks.
  CHECK(anthropic_result.usage["server_tool_use"]["web_search_requests"] == 1);
  // The ordinary call still executes; only the hosted one is held back.
  CHECK(CollectToolCalls(anthropic_calls, anthropic_result));
  CHECK(anthropic_result.tool_calls.size() == 1);
  CHECK(anthropic_result.tool_calls[0].name == "read_path");
  // Both server-tool blocks stay in the replay a pause_turn resends.
  CHECK(anthropic_result.replay["content"].size() == 3);
  CHECK(anthropic_result.replay["content"][0]["type"] == "server_tool_use");
  CHECK(anthropic_result.replay["content"][1]["type"] ==
        "web_search_tool_result");

  // An error record in place of results is a failed search.
  ChatResult error_result;
  std::map<int, ToolCall> error_calls;
  WireStreamState error_state;
  WireStreamDelta error_delta = DecodeWireStreamEvent(
      WireApi::kAnthropicMessages,
      JsonDump(json{{"type", "content_block_start"},
                    {"index", 0},
                    {"content_block",
                     {{"type", "web_search_tool_result"},
                      {"tool_use_id", "srvtoolu_2"},
                      {"content",
                       {{"type", "web_search_tool_result_error"},
                        {"error_code", "unavailable"}}}}}}),
      error_result, error_calls, error_state);
  REQUIRE(error_delta.hosted_tool.has_value());
  CHECK(error_delta.hosted_tool->phase == HostedToolPhase::kFailed);
  CHECK(HostedToolJson(*error_delta.hosted_tool).contains("source_count") ==
        false);
}

// Usage and citation payloads are provider-controlled, and under
// -fno-exceptions an operator[] chain through a scalar aborts the process.
// Wrong-typed fields must decode into a sane result instead.
void TestWireStreamMalformedValues() {
  ChatResult responses_result;
  std::map<int, ToolCall> responses_calls;
  WireStreamState responses_state;
  auto responses_event = [&](const json& value) {
    DecodeWireStreamEvent(WireApi::kResponses, JsonDump(value),
                          responses_result, responses_calls, responses_state);
  };
  responses_event(
      {{"type", "response.output_item.done"},
       {"output_index", 0},
       {"item", {{"type", "web_search_call"}, {"id", "search-1"}}}});
  responses_event({{"type", "response.completed"},
                   {"response",
                    {{"status", "completed"},
                     {"usage",
                      {{"input_tokens", 3},
                       {"server_tool_use_details", "not-an-object"}}}}}});
  CHECK(responses_result
            .usage["server_tool_use_details"]["web_search_requests"] == 1);
  CHECK(responses_result.usage["input_tokens"] == 3);

  // A scalar usage object is replaced wholesale rather than indexed into.
  ChatResult scalar_result;
  std::map<int, ToolCall> scalar_calls;
  WireStreamState scalar_state;
  auto scalar_event = [&](const json& value) {
    DecodeWireStreamEvent(WireApi::kResponses, JsonDump(value), scalar_result,
                          scalar_calls, scalar_state);
  };
  scalar_event({{"type", "response.output_item.done"},
                {"output_index", 0},
                {"item", {{"type", "web_search_call"}, {"id", "search-1"}}}});
  scalar_event({{"type", "response.completed"},
                {"response", {{"status", "completed"}, {"usage", "none"}}}});
  CHECK(scalar_result.usage["server_tool_use_details"]["web_search_requests"] ==
        1);

  ChatResult anthropic_result;
  std::map<int, ToolCall> anthropic_calls;
  WireStreamState anthropic_state;
  std::string anthropic_answer;
  auto anthropic_event = [&](const json& value) {
    anthropic_answer += DecodeWireStreamEvent(WireApi::kAnthropicMessages,
                                              JsonDump(value), anthropic_result,
                                              anthropic_calls, anthropic_state)
                            .content;
  };
  anthropic_event(
      {{"type", "message_start"},
       {"message",
        {{"usage",
          {{"input_tokens", 5}, {"server_tool_use", "not-an-object"}}}}}});
  anthropic_event({{"type", "content_block_start"},
                   {"index", 0},
                   {"content_block",
                    {{"type", "server_tool_use"}, {"name", "web_search"}}}});
  anthropic_event(
      {{"type", "content_block_start"},
       {"index", 1},
       {"content_block",
        {{"type", "text"}, {"text", ""}, {"citations", "not-an-array"}}}});
  // A wrong-typed citations field is left alone; a scalar citation is dropped.
  anthropic_event({{"type", "content_block_delta"},
                   {"index", 1},
                   {"delta",
                    {{"type", "citations_delta"},
                     {"citation",
                      {{"type", "web_search_result_location"},
                       {"url", "https://example.test/cpp"},
                       {"cited_text", "source"}}}}}});
  anthropic_event(
      {{"type", "content_block_delta"},
       {"index", 1},
       {"delta", {{"type", "citations_delta"}, {"citation", "scalar"}}}});
  anthropic_event({{"type", "content_block_delta"},
                   {"index", 1},
                   {"delta", {{"type", "text_delta"}, {"text", "answer"}}}});
  anthropic_event({{"type", "message_stop"}});
  CHECK(anthropic_answer == "answer");
  CHECK(anthropic_result.usage["server_tool_use"]["web_search_requests"] == 1);
  CHECK(anthropic_result.usage["input_tokens"] == 5);
  CHECK(anthropic_result.annotations.size() == 1);
  const json* content = JsonArray(anthropic_result.replay, "content");
  CHECK(content != nullptr && content->size() == 2);
  if (content && content->size() == 2) {
    CHECK((*content)[1]["citations"] == "not-an-array");
  }

  // A citations field the provider left absent still collects normally.
  ChatResult fresh_result;
  std::map<int, ToolCall> fresh_calls;
  WireStreamState fresh_state;
  auto fresh_event = [&](const json& value) {
    DecodeWireStreamEvent(WireApi::kAnthropicMessages, JsonDump(value),
                          fresh_result, fresh_calls, fresh_state);
  };
  fresh_event({{"type", "content_block_start"},
               {"index", 0},
               {"content_block", {{"type", "text"}, {"text", ""}}}});
  fresh_event(
      {{"type", "content_block_delta"},
       {"index", 0},
       {"delta",
        {{"type", "citations_delta"},
         {"citation",
          {{"url", "https://example.test/cpp"}, {"cited_text", "source"}}}}}});
  fresh_event({{"type", "message_stop"}});
  const json* fresh_content = JsonArray(fresh_result.replay, "content");
  CHECK(fresh_content != nullptr && fresh_content->size() == 1);
  if (fresh_content && !fresh_content->empty()) {
    CHECK((*fresh_content)[0]["citations"].size() == 1);
  }
}

}  // namespace uagent
