// Copyright 2026 Timon Gentzsch

#include "include/api/openai_stream.h"

#include <limits>
#include <map>
#include <string>
#include <utility>

#include "include/api/retry.h"
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

void AddAnnotations(const json& annotations, ChatResult& result) {
  if (!annotations.is_array()) return;
  for (const json& annotation : annotations) {
    if (annotation.is_object()) {
      result.annotations.push_back(annotation);
      result.semantic_progress = true;
    }
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

}  // namespace

OpenAiStreamDelta DecodeOpenAiStreamEvent(std::string_view data,
                                          ChatResult& result,
                                          std::map<int, ToolCall>& tool_calls) {
  OpenAiStreamDelta delta;
  size_t begin = data.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return delta;
  data = data.substr(begin, data.find_last_not_of(" \t\r\n") - begin + 1);
  if (data == "[DONE]") return delta;

  json value = json::parse(data.begin(), data.end(), nullptr, false);
  if (value.is_discarded()) return delta;
  json nested;
  const json* envelope = &value;
  if (!value.contains("error") && value.contains("message") &&
      value["message"].is_string()) {
    nested = json::parse(value["message"].get<std::string>(), nullptr, false);
    if (nested.is_object()) envelope = &nested;
  }
  if (envelope->contains("error")) {
    result.retryable = ApplyRemoteError((*envelope)["error"], result);
    result.error = JsonErrorMessage(*envelope, "stream failed");
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
  if (!direct_reasoning.empty()) {
    delta.reasoning = std::move(direct_reasoning);
    delta.activity = true;
  } else if (!details_reasoning.empty()) {
    // Some OpenRouter providers stream normalized reasoning only through
    // reasoning_details, without the convenience `reasoning` field.
    delta.reasoning = std::move(details_reasoning);
    delta.activity = true;
  }
  if (event_delta.contains("content") && event_delta["content"].is_string()) {
    delta.content = event_delta["content"].get<std::string>();
    delta.activity = delta.activity || !delta.content.empty();
  }
  if (!event_delta.contains("tool_calls") ||
      !event_delta["tool_calls"].is_array()) {
    return delta;
  }

  delta.activity = delta.activity || !event_delta["tool_calls"].empty();
  for (const json& tool_call : event_delta["tool_calls"]) {
    if (!tool_call.is_object()) continue;
    int64_t index = JsonValue(tool_call, "index", int64_t{0});
    if (index < 0 || index > std::numeric_limits<int>::max()) continue;
    ToolCall& target = tool_calls[static_cast<int>(index)];
    if (tool_call.contains("id") && tool_call["id"].is_string()) {
      MergeStreamIdentity(target.id, tool_call["id"].get<std::string>());
    }
    if (!tool_call.contains("function") || !tool_call["function"].is_object()) {
      continue;
    }
    const json& function = tool_call["function"];
    if (function.contains("name") && function["name"].is_string()) {
      MergeStreamIdentity(target.name, function["name"].get<std::string>());
    }
    if (function.contains("arguments") && function["arguments"].is_string()) {
      target.args += function["arguments"].get<std::string>();
    }
  }
  return delta;
}

}  // namespace uagent
