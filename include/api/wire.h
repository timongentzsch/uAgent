// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_WIRE_H_
#define UAGENT_INCLUDE_API_WIRE_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "include/api/capabilities.h"
#include "include/api/types.h"
#include "include/core/json.h"

namespace uagent {

inline constexpr char kWireReplayField[] = "_uagent_wire_replay";

struct WireRequest {
  const std::string& model;
  const json& messages;
  const json& tool_schemas;
  std::string_view reasoning_effort;
  int64_t max_output_tokens = -1;
  bool native_tools = true;
  bool parallel_tools = true;
  bool stream_usage = false;
  bool native_web_search = false;
  bool function_web_search = true;
};

bool WireSupportsHostedTool(WireApi wire_api, HostedTool tool);
json EncodeWireRequest(WireApi wire_api, const WireRequest& request);
std::string_view WireEndpoint(WireApi wire_api);

struct WireStreamDelta {
  std::string content;
  std::string reasoning;
  bool activity = false;
};

struct ResponsesStreamState {
  std::map<std::string, int> item_slots;
  std::map<int, json> replay_items;
  int64_t web_searches = 0;
};

struct AnthropicBlockState {
  json block = json::object();
  std::string input_json;
};

struct AnthropicStreamState {
  std::map<int, AnthropicBlockState> blocks;
  int64_t web_searches = 0;
};

struct WireStreamState {
  ResponsesStreamState responses;
  AnthropicStreamState anthropic;
};

WireStreamDelta DecodeWireStreamEvent(WireApi wire_api, std::string_view data,
                                      ChatResult& result,
                                      std::map<int, ToolCall>& tool_calls,
                                      WireStreamState& state);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_WIRE_H_
