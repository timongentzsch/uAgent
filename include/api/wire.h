// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_WIRE_H_
#define UAGENT_INCLUDE_API_WIRE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/api/capabilities.h"
#include "include/api/types.h"
#include "include/core/json.h"

namespace uagent {

inline constexpr char kWireReplayField[] = "_uagent_wire_replay";
// Harness-only metadata: a successful, untruncated read's [path, first, last].
inline constexpr char kReadRangeField[] = "_uagent_read_range";

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
  bool include_web_search_sources = false;
};

bool WireSupportsHostedTool(WireApi wire_api, HostedTool tool);
json EncodeWireRequest(WireApi wire_api, const WireRequest& request);

// Shares the wire encoders above, retaining only unchanged per-message
// encodings and the current tool set. No caller-maintained revision is needed.
class WireRequestCache {
 public:
  json Encode(WireApi wire_api, const WireRequest& request);
  std::string Serialize(const json& body) const;

 private:
  struct Message {
    json source;
    std::string body, role, system;
    bool valid = false;
  };
  WireApi wire_api_ = WireApi::kChatCompletions;
  std::vector<Message> messages_;
  json tool_key_, schemas_;
  bool active_ = false;
  std::string encoded_messages_, encoded_tools_;
};
std::string_view WireEndpoint(WireApi wire_api);

enum class HostedToolPhase : uint8_t {
  kNone,
  kStarted,
  kSearching,
  kCompleted,
  kFailed,
};

// One lifecycle step of a tool the *provider* ran. Deliberately not a
// ToolCall: nothing here is dispatched, approved, or answered locally, and
// routing a provider's own search through the tool loop would make it look
// like a round this agent still owes a result for.
struct HostedToolDelta {
  HostedTool tool = HostedTool::kWebSearch;
  HostedToolPhase phase = HostedToolPhase::kNone;
  std::string id;
  int64_t source_count = -1;  // negative when the provider did not say
};

struct WireStreamDelta {
  std::string content;
  std::string reasoning;
  bool activity = false;
  std::optional<HostedToolDelta> hosted_tool;
};

// The observable payload of a hosted-tool step. The query is deliberately
// absent: it would be the one piece of model-chosen prose on a spine whose
// other transient events carry none, and nothing downstream needs it.
json HostedToolJson(const HostedToolDelta& delta);

// Both providers report a finished search more than once -- Responses as
// `web_search_call.completed` and again as `output_item.done` -- so phases are
// tracked per search and only forward steps are reported. Searches are counted
// through the same key, which is what keeps one search worth one request.
struct HostedToolState {
  std::map<std::string, HostedToolPhase> phases;
  int64_t web_searches = 0;
};

struct ResponsesStreamState {
  std::map<std::string, int> item_slots;
  std::map<int, json> replay_items;
  HostedToolState hosted;
};

struct AnthropicBlockState {
  json block = json::object();
  std::string input_json;
};

struct AnthropicStreamState {
  std::map<int, AnthropicBlockState> blocks;
  HostedToolState hosted;
  // Correlates a result block back to its request when the provider omitted
  // the tool_use_id.
  std::string open_search;
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
