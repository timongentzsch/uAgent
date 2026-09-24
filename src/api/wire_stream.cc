// Copyright 2026 Timon Gentzsch

#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "include/api/retry.h"
#include "include/api/wire.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

void MergeStreamIdentity(std::string& target, const std::string& fragment) {
  if (fragment.empty()) return;
  if (target.empty() || fragment.starts_with(target)) {
    target = fragment;
  } else if (fragment != target) {
    target += fragment;
  }
}

void AddStreamAnnotation(const json& annotation, ChatResult& result) {
  if (!annotation.is_object()) return;
  for (const json& existing : result.annotations) {
    if (existing == annotation) return;
  }
  result.annotations.push_back(annotation);
  result.semantic_progress = true;
}

// OpenAI-compatible streams number parallel tool calls with `index`. A
// provider that omits it would otherwise pile every fragment into slot 0 and
// yield one call whose arguments merge two schemas, so key on the call id
// instead: a known id continues its slot, an unseen id opens the next one, and
// an anonymous fragment continues the newest slot.
int StreamToolSlot(const std::map<int, ToolCall>& calls,
                   const std::string& id) {
  if (calls.empty()) return 0;
  if (id.empty()) return calls.rbegin()->first;
  for (const auto& [slot, call] : calls) {
    if (call.id == id) return slot;
  }
  return calls.rbegin()->first + 1;
}

void AddAnnotations(const json& annotations, ChatResult& result) {
  if (!annotations.is_array()) return;
  for (const json& annotation : annotations) {
    AddStreamAnnotation(annotation, result);
  }
}

std::string AddReasoningDetails(const json& details, ChatResult& result) {
  std::string streamed_text;
  if (!details.is_array()) return streamed_text;
  for (const json& detail : details) {
    if (!detail.is_object()) continue;
    std::string type = JsonValue(detail, "type", "");
    if (type == "reasoning.text" && detail.contains("text") &&
        detail["text"].is_string()) {
      streamed_text += detail["text"].get<std::string>();
    } else if (type == "reasoning.summary" && detail.contains("summary") &&
               detail["summary"].is_string()) {
      streamed_text += detail["summary"].get<std::string>();
    }
    int64_t index = JsonValue(detail, "index", -1);
    json* target = nullptr;
    if (index >= 0) {
      for (json& existing : result.reasoning_details) {
        if (existing.is_object() && JsonValue(existing, "index", -1) == index) {
          target = &existing;
          break;
        }
      }
    } else if (!type.empty()) {
      for (auto existing = result.reasoning_details.rbegin();
           existing != result.reasoning_details.rend(); ++existing) {
        if (existing->is_object() && JsonValue(*existing, "type", "") == type) {
          target = &*existing;
          break;
        }
      }
    }
    if (!target) {
      result.reasoning_details.push_back(detail);
      continue;
    }
    for (const auto& [key, value] : detail.items()) {
      if ((key == "text" || key == "summary" || key == "data" ||
           key == "signature") &&
          value.is_string() && target->contains(key) &&
          (*target)[key].is_string()) {
        (*target)[key] =
            (*target)[key].get<std::string>() + value.get<std::string>();
      } else if (!target->contains(key) || (*target)[key].is_null()) {
        (*target)[key] = value;
      }
    }
  }
  return streamed_text;
}

// OpenAI Chat Completions and compatible providers.
WireStreamDelta DecodeChatCompletionsEvent(
    const json& value, ChatResult& result,
    std::map<int, ToolCall>& tool_calls) {
  WireStreamDelta delta;
  json nested;
  const json* envelope = &value;
  if (!value.contains("error") && value.contains("message") &&
      value["message"].is_string()) {
    nested = json::parse(value["message"].get<std::string>(), nullptr, false);
    if (nested.is_object()) envelope = &nested;
  }
  if (envelope->contains("error")) {
    result.retryable = ApplyRemoteError((*envelope)["error"], result) ||
                       BarrenStreamError(result);
    result.error = JsonErrorMessage(*envelope, "stream failed");
    return delta;
  }
  // Some providers inject the bare {"type":"…error","message":"…"} shape
  // rather than the documented envelope. Falling through would drop it as an
  // unrecognized frame and end the stream with no diagnosis at all.
  if (!value.contains("choices") && envelope->contains("message") &&
      (*envelope)["message"].is_string() &&
      JsonValue(*envelope, "type", "").ends_with("error")) {
    result.retryable =
        ApplyRemoteError(*envelope, result) || BarrenStreamError(result);
    result.error = (*envelope)["message"].get<std::string>();
    return delta;
  }
  if (value.contains("usage") && !value["usage"].is_null()) {
    result.usage = value["usage"];
    result.semantic_progress = true;
  }
  if (!value.contains("choices") || !value["choices"].is_array() ||
      value["choices"].empty() || !value["choices"][0].is_object()) {
    return delta;
  }

  const json& choice = value["choices"][0];
  if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
    result.finish_reason = choice["finish_reason"].get<std::string>();
    result.stop_cause = ClassifyResponseStop(result.finish_reason);
  }
  if (choice.contains("annotations")) {
    AddAnnotations(choice["annotations"], result);
  }
  if (choice.contains("message") && choice["message"].is_object() &&
      choice["message"].contains("annotations")) {
    AddAnnotations(choice["message"]["annotations"], result);
  }
  if (!choice.contains("delta") || !choice["delta"].is_object()) return delta;

  const json& event_delta = choice["delta"];
  if (event_delta.contains("annotations")) {
    AddAnnotations(event_delta["annotations"], result);
  }
  std::string details_reasoning;
  if (event_delta.contains("reasoning_details") &&
      event_delta["reasoning_details"].is_array()) {
    result.reasoning_details_field = true;
    details_reasoning =
        AddReasoningDetails(event_delta["reasoning_details"], result);
    delta.activity =
        delta.activity || !event_delta["reasoning_details"].empty();
  }
  std::string direct_reasoning;
  if (event_delta.contains("reasoning_content") &&
      event_delta["reasoning_content"].is_string()) {
    result.reasoning_content_field = true;
  }
  if (event_delta.contains("reasoning") &&
      event_delta["reasoning"].is_string()) {
    result.reasoning_field = true;
    direct_reasoning = event_delta["reasoning"].get<std::string>();
  }
  if (direct_reasoning.empty() && event_delta.contains("reasoning_content") &&
      event_delta["reasoning_content"].is_string()) {
    direct_reasoning = event_delta["reasoning_content"].get<std::string>();
  }
  if (!details_reasoning.empty()) {
    for (const json& detail : event_delta["reasoning_details"]) {
      const std::string type = JsonValue(detail, "type", "");
      const bool summary = type == "reasoning.summary";
      if (!summary && type != "reasoning.text") continue;
      const std::string text =
          JsonValue(detail, summary ? "summary" : "text", "");
      if (text.empty()) continue;
      const std::string part =
          type + "/" + std::to_string(JsonValue(detail, "index", int64_t{-1}));
      AddReasoningDelta(delta, part, summary ? "summary" : "reasoning", text);
    }
  } else if (!direct_reasoning.empty()) {
    AddReasoningDelta(delta, "reasoning", "reasoning",
                      std::move(direct_reasoning));
  }
  if (event_delta.contains("content") && event_delta["content"].is_string()) {
    delta.content = event_delta["content"].get<std::string>();
    delta.activity = delta.activity || !delta.content.empty();
  }
  if (!event_delta.contains("tool_calls") ||
      !event_delta["tool_calls"].is_array()) {
    return delta;
  }

  delta.tool_arguments = !event_delta["tool_calls"].empty();
  delta.activity = delta.activity || delta.tool_arguments;
  for (const json& tool_call : event_delta["tool_calls"]) {
    if (!tool_call.is_object()) continue;
    int64_t index = JsonValue(tool_call, "index", int64_t{-1});
    if (index > std::numeric_limits<int>::max()) continue;
    std::string id;
    if (tool_call.contains("id") && tool_call["id"].is_string()) {
      id = tool_call["id"].get<std::string>();
    }
    if (index < 0) index = StreamToolSlot(tool_calls, id);
    ToolCall& target = tool_calls[static_cast<int>(index)];
    if (!id.empty()) MergeStreamIdentity(target.id, id);
    const json* found = JsonObject(tool_call, "function");
    if (!found) continue;
    const json& function = *found;
    if (function.contains("name") && function["name"].is_string()) {
      MergeStreamIdentity(target.name, function["name"].get<std::string>());
    }
    if (function.contains("arguments") && function["arguments"].is_string()) {
      target.args += function["arguments"].get<std::string>();
    }
  }
  return delta;
}

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

void AddAnnotationsFromMessage(const json& item, ChatResult& result) {
  const json* content = JsonArray(item, "content");
  if (!content) return;
  for (const json& part : *content) {
    const json* annotations = JsonArray(part, "annotations");
    if (!annotations) continue;
    for (const json& annotation : *annotations) {
      AddStreamAnnotation(annotation, result);
    }
  }
}

// Hosted search observed in the stream but absent from the provider's usage
// (the Responses and Anthropic dialects report it under different keys).
void BackfillSearchCount(json& usage, const char* key, int64_t searches) {
  if (searches <= 0) return;
  const json* reported = JsonObject(usage, key);
  if (reported && JsonValue(*reported, "web_search_requests", int64_t{0}) > 0) {
    return;
  }
  EnsureObject(usage, key)["web_search_requests"] = searches;
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

int HostedToolRank(HostedToolPhase phase) {
  switch (phase) {
    case HostedToolPhase::kNone:
      return -1;
    case HostedToolPhase::kStarted:
      return 0;
    case HostedToolPhase::kSearching:
      return 1;
    case HostedToolPhase::kCompleted:
    case HostedToolPhase::kFailed:
      return 2;
  }
  return -1;
}

// What a search is tracked under. Providers always name one, but a stream
// that omits the id still has a stable slot, and correlating by position is
// what keeps a nameless search from merging with the next one.
std::string HostedToolKey(const std::string& id, int slot) {
  return id.empty() ? "#" + std::to_string(slot) : id;
}

HostedToolPhase SearchStopPhase(std::string_view status) {
  return status == "failed" || status == "incomplete"
             ? HostedToolPhase::kFailed
             : HostedToolPhase::kCompleted;
}

// Record one step of a hosted search, reporting it only when it advances that
// search. Counting rides the same key, so a provider repeating a terminal
// event neither doubles the request count nor the step the presenter sees.
void MarkHostedTool(WireStreamDelta& delta, HostedToolState& state,
                    const std::string& key, std::string id,
                    HostedToolPhase phase, int64_t source_count) {
  HostedToolPhase& tracked = state.phases[key];
  if (HostedToolRank(phase) <= HostedToolRank(tracked)) return;
  if (tracked == HostedToolPhase::kNone) ++state.web_searches;
  tracked = phase;
  delta.hosted_tool = HostedToolDelta{HostedTool::kWebSearch, phase,
                                      std::move(id), source_count};
}

int64_t JsonArraySize(const json& value, const char* key) {
  const json* array = JsonArray(value, key);
  return array ? static_cast<int64_t>(array->size()) : -1;
}

int ResponsesSlot(const json& value, ResponsesStreamState& state,
                  const std::map<int, ToolCall>& calls) {
  int64_t output_index = JsonValue(value, "output_index", int64_t{-1});
  if (output_index >= 0 &&
      output_index <= static_cast<int64_t>(std::numeric_limits<int>::max())) {
    int slot = static_cast<int>(output_index);
    std::string item_id = JsonValue(
        value, "item_id",
        JsonValue(JsonValue(value, "item", json::object()), "id", ""));
    if (!item_id.empty()) state.item_slots[item_id] = slot;
    return slot;
  }
  std::string item_id =
      JsonValue(value, "item_id",
                JsonValue(JsonValue(value, "item", json::object()), "id", ""));
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

void ResponsesSummary(const json& item, int slot, bool complete,
                      WireStreamDelta& delta) {
  if (const json* summary = JsonArray(item, "summary")) {
    for (size_t index = 0; index < summary->size(); ++index) {
      AddReasoningDelta(
          delta,
          "summary/" + std::to_string(slot) + "/" + std::to_string(index),
          "summary", JsonValue((*summary)[index], "text", ""), complete, true);
    }
  }
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
      type == "response.reasoning_text.delta" ||
      type == "response.reasoning_summary_text.done" ||
      type == "response.reasoning_text.done" ||
      type == "response.reasoning_summary_part.added" ||
      type == "response.reasoning_summary_part.done") {
    const bool summary = type.find("summary") != std::string::npos;
    const std::string kind = summary ? "summary" : "reasoning";
    const int slot = ResponsesSlot(value, state, calls);
    const int64_t part = JsonValue(
        value, summary ? "summary_index" : "content_index", int64_t{0});
    const bool done = type.ends_with(".done");
    const bool part_event = type.find("_part.") != std::string::npos;
    const std::string text =
        part_event
            ? JsonValue(JsonValue(value, "part", json::object()), "text", "")
            : JsonValue(value, done ? "text" : "delta", "");
    AddReasoningDelta(
        delta, kind + "/" + std::to_string(slot) + "/" + std::to_string(part),
        kind, text, done, done || part_event);
    result.reasoning_field = true;
    return delta;
  }
  if (type == "response.output_text.annotation.added") {
    if (value.contains("annotation")) {
      AddStreamAnnotation(value["annotation"], result);
    }
    delta.activity = true;
    return delta;
  }
  if (type == "response.function_call_arguments.delta" ||
      type == "response.function_call_arguments.done") {
    delta.tool_arguments = true;
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
      delta.tool_arguments = true;
      int slot = ResponsesSlot(value, state, calls);
      ToolCall& call = calls[slot];
      MergeStreamIdentity(call.id, JsonValue(*item, "call_id", ""));
      MergeStreamIdentity(call.name, JsonValue(*item, "name", ""));
      if (type.ends_with(".done")) {
        std::string arguments = JsonValue(*item, "arguments", "");
        if (!arguments.empty()) call.args = std::move(arguments);
      }
    } else if (item_type == "reasoning") {
      const int slot = ResponsesSlot(value, state, calls);
      ResponsesSummary(*item, slot, type.ends_with(".done"), delta);
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
    }
    if (item_type == "web_search_call") {
      const json* action = JsonObject(*item, "action");
      std::string id = JsonValue(*item, "id", "");
      // The key reads `id` and the same call then moves it. Argument
      // evaluation order is unspecified, so the key is built first rather
      // than racing the move.
      std::string key = HostedToolKey(id, ResponsesSlot(value, state, calls));
      MarkHostedTool(delta, state.hosted, key, std::move(id),
                     type.ends_with(".added")
                         ? HostedToolPhase::kStarted
                         : SearchStopPhase(JsonValue(*item, "status", "")),
                     action ? JsonArraySize(*action, "sources") : -1);
    }
    delta.activity = true;
    return delta;
  }
  if (type == "response.completed") {
    const json* response = JsonObject(value, "response");
    if (response) {
      if (const json* output = JsonArray(*response, "output")) {
        for (size_t index = 0; index < output->size(); ++index) {
          const json& item = (*output)[index];
          if (JsonValue(item, "type", "") != "reasoning") continue;
          const int slot = ResponsesSlot(
              {{"output_index", index}, {"item", item}}, state, calls);
          ResponsesSummary(item, slot, true, delta);
          RecordResponsesReplay(slot, item, state, result);
        }
      }
      if (response->contains("usage")) result.usage = (*response)["usage"];
      result.finish_reason = JsonValue(*response, "status", "completed");
      result.stop_cause = ClassifyResponseStop(result.finish_reason);
    }
    BackfillSearchCount(result.usage, "server_tool_use_details",
                        state.hosted.web_searches);
    delta.activity = true;
    return delta;
  }
  if (type.starts_with("response.web_search_call.")) {
    std::string id = JsonValue(value, "item_id", "");
    HostedToolPhase phase =
        type.ends_with(".searching")   ? HostedToolPhase::kSearching
        : type.ends_with(".completed") ? HostedToolPhase::kCompleted
                                       : HostedToolPhase::kStarted;
    std::string key = HostedToolKey(id, ResponsesSlot(value, state, calls));
    MarkHostedTool(delta, state.hosted, key, std::move(id), phase,
                   /*source_count=*/-1);
    delta.activity = true;
    return delta;
  }
  if (type == "response.created" || type == "response.in_progress") {
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
      BackfillSearchCount(result.usage, "server_tool_use",
                          state.hosted.web_searches);
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
      AddReasoningDelta(delta, "thinking/" + std::to_string(index), "reasoning",
                        JsonValue(*started, "thinking", ""));
      result.reasoning_field = true;
    } else if (block_type == "tool_use") {
      delta.tool_arguments = true;
      ToolCall& call = calls[index];
      call.id = JsonValue(*started, "id", "");
      call.name = JsonValue(*started, "name", "");
      if (started->contains("input") && !(*started)["input"].empty()) {
        call.args = JsonDump((*started)["input"]);
      }
    } else if (block_type == "server_tool_use" &&
               JsonValue(*started, "name", "") == "web_search") {
      std::string id = JsonValue(*started, "id", "");
      state.open_search = HostedToolKey(id, index);
      MarkHostedTool(delta, state.hosted, state.open_search, std::move(id),
                     HostedToolPhase::kSearching, /*source_count=*/-1);
    } else if (block_type == "web_search_tool_result") {
      // An error record is a failed search, not a completed one with no
      // sources; Anthropic reports it in place of the result array.
      const json* failure = JsonObject(*started, "content");
      std::string id = JsonValue(*started, "tool_use_id", "");
      std::string key = id.empty() && !state.open_search.empty()
                            ? state.open_search
                            : HostedToolKey(id, index);
      MarkHostedTool(delta, state.hosted, key, std::move(id),
                     failure && JsonValue(*failure, "type", "") ==
                                    "web_search_tool_result_error"
                         ? HostedToolPhase::kFailed
                         : HostedToolPhase::kCompleted,
                     JsonArraySize(*started, "content"));
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
      delta.tool_arguments = true;
      std::string fragment = JsonValue(*event_delta, "partial_json", "");
      block.input_json += fragment;
      calls[index].args += fragment;
    } else if (delta_type == "thinking_delta") {
      AddReasoningDelta(delta, "thinking/" + std::to_string(index), "reasoning",
                        JsonValue(*event_delta, "thinking", ""));
      result.reasoning_field = true;
      block.block["thinking"] =
          JsonValue(block.block, "thinking", "") + delta.reasoning;
    } else if (delta_type == "signature_delta") {
      block.block["signature"] = JsonValue(block.block, "signature", "") +
                                 JsonValue(*event_delta, "signature", "");
    } else if (delta_type == "citations_delta" &&
               event_delta->contains("citation")) {
      const json& citation = (*event_delta)["citation"];
      if (citation.is_object()) {
        // The block's shape is the provider's: a "citations" that arrived as
        // anything but an array has no push_back to call.
        json& citations = block.block["citations"];
        if (citations.is_null()) citations = json::array();
        if (citations.is_array()) citations.push_back(citation);
        json normalized = AnthropicCitation(citation);
        if (normalized.is_object()) AddStreamAnnotation(normalized, result);
      }
    }
    delta.activity = true;
    return delta;
  }
  if (type == "content_block_stop") {
    if (JsonValue(block.block, "type", "") == "thinking") {
      AddReasoningDelta(delta, "thinking/" + std::to_string(index), "reasoning",
                        "", true);
    }
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

void AddReasoningDelta(WireStreamDelta& delta, std::string part,
                       std::string kind, std::string text, bool complete,
                       bool snapshot) {
  delta.reasoning += text;
  delta.activity = delta.activity || !text.empty();
  delta.reasoning_parts.push_back(
      {std::move(part), std::move(kind), std::move(text), complete, snapshot});
}

json HostedToolJson(const HostedToolDelta& delta) {
  const char* phase = "none";
  switch (delta.phase) {
    case HostedToolPhase::kNone:
      break;
    case HostedToolPhase::kStarted:
      phase = "started";
      break;
    case HostedToolPhase::kSearching:
      phase = "searching";
      break;
    case HostedToolPhase::kCompleted:
      phase = "completed";
      break;
    case HostedToolPhase::kFailed:
      phase = "failed";
      break;
  }
  // Named rather than initialised to the same literal the switch assigns: a
  // dead store there would hide a future enumerator that falls through.
  const char* tool = nullptr;
  switch (delta.tool) {
    case HostedTool::kWebSearch:
      tool = "web_search";
      break;
  }
  json value = {{"tool", tool}, {"id", delta.id}, {"phase", phase}};
  if (delta.source_count >= 0) value["source_count"] = delta.source_count;
  return value;
}

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
  json value = ParseEvent(data);
  if (value.is_discarded() || value.is_null()) return {};
  if (wire_api == WireApi::kChatCompletions) {
    return DecodeChatCompletionsEvent(value, result, tool_calls);
  }
  if (wire_api == WireApi::kResponses) {
    return DecodeResponsesEvent(value, result, tool_calls, state.responses);
  }
  return DecodeAnthropicEvent(value, result, tool_calls, state.anthropic);
}

}  // namespace uagent
