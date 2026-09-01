// Copyright 2026 Timon Gentzsch

#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "include/api/openai_stream.h"
#include "include/api/retry.h"
#include "include/api/wire.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

void MergeObject(json& target, const json& update) {
  if (!update.is_object()) return;
  if (!target.is_object()) target = json::object();
  for (const auto& [key, value] : update.items()) {
    if (value.is_object() && target.contains(key) && target[key].is_object()) {
      MergeObject(target[key], value);
    } else {
      target[key] = value;
    }
  }
}

void AddAnnotation(const json& annotation, ChatResult& result) {
  if (!annotation.is_object()) return;
  for (const json& existing : result.annotations) {
    if (existing == annotation) return;
  }
  result.annotations.push_back(annotation);
  result.semantic_progress = true;
}

void AddAnnotationsFromMessage(const json& item, ChatResult& result) {
  const json* content = JsonArray(item, "content");
  if (!content) return;
  for (const json& part : *content) {
    const json* annotations = JsonArray(part, "annotations");
    if (!annotations) continue;
    for (const json& annotation : *annotations) {
      AddAnnotation(annotation, result);
    }
  }
}

json ParseEvent(std::string_view data) {
  size_t begin = data.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return json(nullptr);
  size_t end = data.find_last_not_of(" \t\r\n");
  data = data.substr(begin, end - begin + 1);
  if (data == "[DONE]") return json(nullptr);
  return json::parse(data.begin(), data.end(), nullptr, false);
}

void ApplyStreamError(const json& error, ChatResult& result,
                      std::string fallback) {
  if (error.is_object()) {
    result.retryable =
        ApplyRemoteError(error, result) || BarrenStreamError(result);
    result.error = JsonValue(error, "message", std::move(fallback));
  } else {
    result.retryable = BarrenStreamError(result);
    result.error = std::move(fallback);
  }
}

int ResponsesSlot(const json& value, ResponsesStreamState& state,
                  const std::map<int, ToolCall>& calls) {
  int64_t output_index = JsonValue(value, "output_index", int64_t{-1});
  if (output_index >= 0 &&
      output_index <= static_cast<int64_t>(std::numeric_limits<int>::max())) {
    int slot = static_cast<int>(output_index);
    std::string item_id = JsonValue(value, "item_id", "");
    if (!item_id.empty()) state.item_slots[item_id] = slot;
    return slot;
  }
  std::string item_id = JsonValue(value, "item_id", "");
  if (!item_id.empty()) {
    auto found = state.item_slots.find(item_id);
    if (found != state.item_slots.end()) return found->second;
  }
  return calls.empty() ? 0 : calls.rbegin()->first + 1;
}

void RecordResponsesReplay(int slot, const json& item,
                           ResponsesStreamState& state, ChatResult& result) {
  state.replay_items[slot] = item;
  json items = json::array();
  for (const auto& [index, replay] : state.replay_items) {
    (void)index;
    items.push_back(replay);
  }
  result.replay = {{"wire_api", "responses"}, {"items", std::move(items)}};
}

WireStreamDelta DecodeResponsesEvent(const json& value, ChatResult& result,
                                     std::map<int, ToolCall>& calls,
                                     ResponsesStreamState& state) {
  WireStreamDelta delta;
  if (!value.is_object()) return delta;
  if (value.contains("error")) {
    ApplyStreamError(value["error"], result, "response stream failed");
    return delta;
  }
  const std::string type = JsonValue(value, "type", "");
  if (type == "error") {
    ApplyStreamError(value.contains("error") ? value["error"] : value, result,
                     "response stream failed");
    return delta;
  }
  if (type == "response.failed") {
    const json* response = JsonObject(value, "response");
    const json* error = response ? JsonObject(*response, "error") : nullptr;
    ApplyStreamError(error ? *error : json(nullptr), result,
                     "response stream failed");
    return delta;
  }
  if (type == "response.incomplete") {
    const json* response = JsonObject(value, "response");
    result.incomplete = true;
    if (response) {
      if (response->contains("usage")) result.usage = (*response)["usage"];
      result.stop_details =
          JsonValue(*response, "incomplete_details", json::object());
    }
    result.finish_reason =
        JsonValue(result.stop_details, "reason", "incomplete");
    result.stop_cause = ClassifyResponseStop(result.finish_reason);
    delta.activity = true;
    return delta;
  }
  if (type == "response.output_text.delta") {
    delta.content = JsonValue(value, "delta", "");
    delta.activity = !delta.content.empty();
    return delta;
  }
  if (type == "response.reasoning_summary_text.delta" ||
      type == "response.reasoning_text.delta") {
    delta.reasoning = JsonValue(value, "delta", "");
    result.reasoning_field = true;
    delta.activity = !delta.reasoning.empty();
    return delta;
  }
  if (type == "response.output_text.annotation.added") {
    if (value.contains("annotation")) {
      AddAnnotation(value["annotation"], result);
    }
    delta.activity = true;
    return delta;
  }
  if (type == "response.function_call_arguments.delta" ||
      type == "response.function_call_arguments.done") {
    int slot = ResponsesSlot(value, state, calls);
    ToolCall& call = calls[slot];
    MergeStreamIdentity(call.id, JsonValue(value, "call_id", ""));
    std::string fragment = type.ends_with(".delta")
                               ? JsonValue(value, "delta", "")
                               : JsonValue(value, "arguments", "");
    if (type.ends_with(".done") && !fragment.empty()) {
      call.args = std::move(fragment);
    } else {
      call.args += fragment;
    }
    delta.activity = true;
    return delta;
  }
  if (type == "response.output_item.added" ||
      type == "response.output_item.done") {
    const json* item = JsonObject(value, "item");
    if (!item) return delta;
    const std::string item_type = JsonValue(*item, "type", "");
    if (item_type == "function_call") {
      int slot = ResponsesSlot(value, state, calls);
      ToolCall& call = calls[slot];
      MergeStreamIdentity(call.id, JsonValue(*item, "call_id", ""));
      MergeStreamIdentity(call.name, JsonValue(*item, "name", ""));
      if (type.ends_with(".done")) {
        std::string arguments = JsonValue(*item, "arguments", "");
        if (!arguments.empty()) call.args = std::move(arguments);
      }
    } else if (item_type == "message") {
      AddAnnotationsFromMessage(*item, result);
      if (type.ends_with(".done") && result.content.empty()) {
        const json* content = JsonArray(*item, "content");
        if (content) {
          for (const json& part : *content) {
            if (JsonValue(part, "type", "") == "output_text") {
              delta.content += JsonValue(part, "text", "");
            }
          }
        }
      }
    }
    if (type.ends_with(".done")) {
      RecordResponsesReplay(ResponsesSlot(value, state, calls), *item, state,
                            result);
      if (item_type == "web_search_call") ++state.web_searches;
    }
    delta.activity = true;
    return delta;
  }
  if (type == "response.completed") {
    const json* response = JsonObject(value, "response");
    if (response) {
      if (response->contains("usage")) result.usage = (*response)["usage"];
      result.finish_reason = JsonValue(*response, "status", "completed");
      result.stop_cause = ClassifyResponseStop(result.finish_reason);
    }
    if (state.web_searches > 0) {
      if (!result.usage.is_object()) result.usage = json::object();
      const bool reported =
          result.usage.contains("server_tool_use_details") &&
          result.usage["server_tool_use_details"].is_object() &&
          JsonValue(result.usage["server_tool_use_details"],
                    "web_search_requests", int64_t{0}) > 0;
      if (!reported) {
        result.usage["server_tool_use_details"]["web_search_requests"] =
            state.web_searches;
      }
    }
    delta.activity = true;
    return delta;
  }
  if (type == "response.created" || type == "response.in_progress" ||
      type == "response.web_search_call.in_progress" ||
      type == "response.web_search_call.searching" ||
      type == "response.web_search_call.completed") {
    delta.activity = true;
  }
  return delta;
}

json AnthropicCitation(const json& citation) {
  if (!citation.is_object()) return json(nullptr);
  const std::string url = JsonValue(citation, "url", "");
  if (url.empty()) return json(nullptr);
  return {{"type", "url_citation"},
          {"url", url},
          {"title", JsonValue(citation, "title", "")},
          {"content", JsonValue(citation, "cited_text", "")}};
}

void SetAnthropicReplay(const AnthropicStreamState& state, ChatResult& result) {
  json content = json::array();
  for (const auto& [index, block] : state.blocks) {
    (void)index;
    if (!block.block.empty()) content.push_back(block.block);
  }
  result.replay = {{"wire_api", "anthropic_messages"},
                   {"content", std::move(content)}};
}

WireStreamDelta DecodeAnthropicEvent(const json& value, ChatResult& result,
                                     std::map<int, ToolCall>& calls,
                                     AnthropicStreamState& state) {
  WireStreamDelta delta;
  if (!value.is_object()) return delta;
  const std::string type = JsonValue(value, "type", "");
  if (type == "error") {
    ApplyStreamError(value.contains("error") ? value["error"] : value, result,
                     "Anthropic stream failed");
    return delta;
  }
  if (type == "message_start") {
    const json* message = JsonObject(value, "message");
    if (message && message->contains("usage")) {
      MergeObject(result.usage, (*message)["usage"]);
    }
    delta.activity = true;
    return delta;
  }
  int64_t raw_index = JsonValue(value, "index", int64_t{-1});
  if (raw_index < 0 ||
      raw_index > static_cast<int64_t>(std::numeric_limits<int>::max())) {
    if (type == "message_delta") {
      const json* message_delta = JsonObject(value, "delta");
      if (message_delta) {
        result.finish_reason = JsonValue(*message_delta, "stop_reason", "");
        result.stop_cause = ClassifyResponseStop(result.finish_reason);
      }
      if (value.contains("usage")) MergeObject(result.usage, value["usage"]);
      delta.activity = true;
    } else if (type == "message_stop") {
      SetAnthropicReplay(state, result);
      if (state.web_searches > 0) {
        const json* server = JsonObject(result.usage, "server_tool_use");
        if (!server ||
            JsonValue(*server, "web_search_requests", int64_t{0}) == 0) {
          result.usage["server_tool_use"]["web_search_requests"] =
              state.web_searches;
        }
      }
      delta.activity = true;
    }
    return delta;
  }

  const int index = static_cast<int>(raw_index);
  AnthropicBlockState& block = state.blocks[index];
  if (type == "content_block_start") {
    const json* started = JsonObject(value, "content_block");
    if (!started) return delta;
    block.block = *started;
    const std::string block_type = JsonValue(*started, "type", "");
    if (block_type == "text") {
      delta.content = JsonValue(*started, "text", "");
    } else if (block_type == "thinking") {
      delta.reasoning = JsonValue(*started, "thinking", "");
      result.reasoning_field = true;
    } else if (block_type == "tool_use") {
      ToolCall& call = calls[index];
      call.id = JsonValue(*started, "id", "");
      call.name = JsonValue(*started, "name", "");
      if (started->contains("input") && !(*started)["input"].empty()) {
        call.args = JsonDump((*started)["input"]);
      }
    } else if (block_type == "server_tool_use" &&
               JsonValue(*started, "name", "") == "web_search") {
      ++state.web_searches;
    }
    delta.activity = true;
    return delta;
  }
  if (type == "content_block_delta") {
    const json* event_delta = JsonObject(value, "delta");
    if (!event_delta) return delta;
    const std::string delta_type = JsonValue(*event_delta, "type", "");
    if (delta_type == "text_delta") {
      delta.content = JsonValue(*event_delta, "text", "");
      block.block["text"] = JsonValue(block.block, "text", "") + delta.content;
    } else if (delta_type == "input_json_delta") {
      std::string fragment = JsonValue(*event_delta, "partial_json", "");
      block.input_json += fragment;
      calls[index].args += fragment;
    } else if (delta_type == "thinking_delta") {
      delta.reasoning = JsonValue(*event_delta, "thinking", "");
      result.reasoning_field = true;
      block.block["thinking"] =
          JsonValue(block.block, "thinking", "") + delta.reasoning;
    } else if (delta_type == "signature_delta") {
      block.block["signature"] = JsonValue(block.block, "signature", "") +
                                 JsonValue(*event_delta, "signature", "");
    } else if (delta_type == "citations_delta" &&
               event_delta->contains("citation")) {
      const json& citation = (*event_delta)["citation"];
      block.block["citations"].push_back(citation);
      json normalized = AnthropicCitation(citation);
      if (normalized.is_object()) AddAnnotation(normalized, result);
    }
    delta.activity = true;
    return delta;
  }
  if (type == "content_block_stop") {
    if (JsonValue(block.block, "type", "") == "tool_use") {
      ToolCall& call = calls[index];
      if (call.args.empty() && block.block.contains("input")) {
        call.args = JsonDump(block.block["input"]);
      }
      json parsed = json::parse(call.args, nullptr, false);
      if (!parsed.is_discarded()) block.block["input"] = std::move(parsed);
    }
    SetAnthropicReplay(state, result);
    delta.activity = true;
  }
  return delta;
}

}  // namespace

std::string_view WireEndpoint(WireApi wire_api) {
  switch (wire_api) {
    case WireApi::kChatCompletions:
      return "/chat/completions";
    case WireApi::kResponses:
      return "/responses";
    case WireApi::kAnthropicMessages:
      return "/messages";
  }
  return "/chat/completions";
}

WireStreamDelta DecodeWireStreamEvent(WireApi wire_api, std::string_view data,
                                      ChatResult& result,
                                      std::map<int, ToolCall>& tool_calls,
                                      WireStreamState& state) {
  if (wire_api == WireApi::kChatCompletions) {
    OpenAiStreamDelta decoded =
        DecodeOpenAiStreamEvent(data, result, tool_calls);
    return {std::move(decoded.content), std::move(decoded.reasoning),
            decoded.activity};
  }
  json value = ParseEvent(data);
  if (value.is_discarded() || value.is_null()) return {};
  if (wire_api == WireApi::kResponses) {
    return DecodeResponsesEvent(value, result, tool_calls, state.responses);
  }
  return DecodeAnthropicEvent(value, result, tool_calls, state.anthropic);
}

}  // namespace uagent
