// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_WIRE_H_
#define UAGENT_INCLUDE_API_WIRE_H_
// The provider-neutral request and its encoders for each wire format, plus
// the fields the harness adds to messages.

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
// Harness-only metadata: a prompt-cache breakpoint ends this message.
inline constexpr char kCacheBreakpointField[] = "_uagent_cache_breakpoint";

struct WireRequest {
  const std::string& model;
  const json& messages;
  const json& tool_schemas;
  std::string_view reasoning_effort = {};
  int64_t max_output_tokens = -1;
  bool parallel_tools = true;
  bool stream_usage = false;
  // A search's side request: the endpoint's own search and no other tool.
  bool native_web_search = false;
  // Tools the endpoint runs itself, as written in `hosted_tools`.
  json hosted_tools = json::array();
  bool reasoning_summary = false;
  bool adaptive_thinking = false;
  // The endpoint takes `cache_control` on Chat Completions content parts.
  bool prompt_cache_control = false;
  // Offers the tools but forbids calling one. Anthropic's cache drops the
  // messages when tool_choice changes, so that wire is left to the prompt.
  bool forbid_tool_calls = false;
};

// Whether a replay holds a provider-run tool other than its web search.
bool ReplayHoldsHostedTool(const json& replay);
json EncodeWireRequest(WireApi wire_api, const WireRequest& request);

// Reuses unchanged per-message encodings and the current tool set.
class WireRequestCache {
 public:
  json Encode(WireApi wire_api, const WireRequest& request);
  std::string Serialize(const json& body) const;

 private:
  struct Message {
    json source;
    std::string body, role, system;
    bool valid = false;
    // Whether only system messages preceded it (see AnthropicMessages).
    bool leading = true;
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

// One lifecycle step of a tool the provider ran. Not a ToolCall: nothing
// here is dispatched, approved or answered locally.
struct HostedToolDelta {
  // The provider's name for it: web_search, code_execution, ...
  std::string tool = "web_search";
  HostedToolPhase phase = HostedToolPhase::kNone;
  std::string id;
  int64_t source_count = -1;  // negative when the provider did not say
};

struct ReasoningDelta {
  std::string part, kind, text;
  bool complete = false;
  bool snapshot = false;
};

struct WireStreamDelta {
  std::string content;
  std::string reasoning;
  std::vector<ReasoningDelta> reasoning_parts;
  bool tool_arguments = false;
  bool activity = false;
  // The stream answered without producing anything: it times the first
  // event, and is no progress a retry would repeat.
  bool opened = false;
  std::optional<HostedToolDelta> hosted_tool;
};

void AddReasoningDelta(WireStreamDelta& delta, std::string part,
                       std::string kind, std::string text,
                       bool complete = false, bool snapshot = false);

// The observable payload of a hosted-tool step, without the model's query.
json HostedToolJson(const HostedToolDelta& delta);

// Providers report a finished search more than once, so phases are tracked
// per search and only forward steps are reported or counted.
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
  // A provider-run tool's input as it streams; it is not a call to run here.
  std::string input;
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
