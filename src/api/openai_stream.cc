// Copyright 2026 Timon Gentzsch

#include "include/api/openai_stream.h"

#include <map>
#include <string>
#include <utility>

#include "include/api/retry.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

void AddAnnotations(const json& annotations, ChatResult& result) {
  if (!annotations.is_array()) return;
  for (const json& annotation : annotations) {
    if (annotation.is_object()) {
      result.annotations.push_back(annotation);
      result.semantic_progress = true;
    }
  }
}

void AddReasoningDetails(const json& details, ChatResult& result) {
  if (!details.is_array()) return;
  for (const json& detail : details) {
    if (!detail.is_object()) continue;
    int64_t index = JsonValue(detail, "index", -1);
    json* target = nullptr;
    if (index >= 0) {
      for (json& existing : result.reasoning_details) {
        if (existing.is_object() && JsonValue(existing, "index", -1) == index) {
          target = &existing;
          break;
        }
      }
    } else {
      std::string type = JsonValue(detail, "type", "");
      if (!type.empty()) {
        for (auto existing = result.reasoning_details.rbegin();
             existing != result.reasoning_details.rend(); ++existing) {
          if (existing->is_object() &&
              JsonValue(*existing, "type", "") == type) {
            target = &*existing;
            break;
          }
        }
      }
    }
    if (!target) {
      result.reasoning_details.push_back(detail);
      continue;
    }
    for (const auto& [key, value] : detail.items()) {
      if ((key == "text" || key == "data" || key == "signature") &&
          value.is_string() && target->contains(key) &&
          (*target)[key].is_string()) {
        (*target)[key] =
            (*target)[key].get<std::string>() + value.get<std::string>();
      } else if (!target->contains(key) || (*target)[key].is_null()) {
        (*target)[key] = value;
      }
    }
  }
}

}  // namespace

OpenAiStreamDelta DecodeOpenAiStreamEvent(std::string_view data,
                                          ChatResult& result,
                                          std::map<int, ToolCall>& tool_calls) {
  OpenAiStreamDelta delta;
  std::string payload = Trim(std::string(data));
  if (payload.empty() || payload == "[DONE]") return delta;

  json value = json::parse(payload, nullptr, false);
  if (value.is_discarded()) return delta;
  if (value.contains("error")) {
    const json& error = value["error"];
    result.remote_error_type = JsonValue(error, "type", "");
    result.remote_error_code = JsonValue(error, "code", "");
    result.retryable = RetryableRemoteError(result.remote_error_type,
                                            result.remote_error_code);
    result.error = JsonErrorMessage(value, "stream failed");
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
  if (event_delta.contains("reasoning_details")) {
    AddReasoningDetails(event_delta["reasoning_details"], result);
    delta.activity =
        delta.activity || !event_delta["reasoning_details"].empty();
  }
  const json* reasoning = nullptr;
  if (event_delta.contains("reasoning") &&
      event_delta["reasoning"].is_string()) {
    reasoning = &event_delta["reasoning"];
  } else if (event_delta.contains("reasoning_content") &&
             event_delta["reasoning_content"].is_string()) {
    reasoning = &event_delta["reasoning_content"];
  }
  if (reasoning) {
    delta.reasoning = reasoning->get<std::string>();
    delta.activity = delta.activity || !delta.reasoning.empty();
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
    ToolCall& target = tool_calls[JsonValue(tool_call, "index", int64_t{0})];
    if (tool_call.contains("id") && tool_call["id"].is_string()) {
      target.id += tool_call["id"].get<std::string>();
    }
    if (!tool_call.contains("function") || !tool_call["function"].is_object()) {
      continue;
    }
    const json& function = tool_call["function"];
    if (function.contains("name") && function["name"].is_string()) {
      target.name += function["name"].get<std::string>();
    }
    if (function.contains("arguments") && function["arguments"].is_string()) {
      target.args += function["arguments"].get<std::string>();
    }
  }
  return delta;
}

}  // namespace uagent
