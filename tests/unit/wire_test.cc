// Copyright 2026 Timon Gentzsch

#include "include/api/wire.h"

#include <map>
#include <string>

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
  CHECK(anthropic_result.continue_response);
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
}

}  // namespace uagent
