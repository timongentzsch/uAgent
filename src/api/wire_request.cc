// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "include/api/wire.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

const json* MatchingReplay(const json& message, std::string_view wire_api) {
  const json* replay = JsonObject(message, kWireReplayField);
  return replay && JsonValue(*replay, "wire_api", "") == wire_api ? replay
                                                                  : nullptr;
}

json ChatMessages(const json& messages) {
  if (!messages.is_array()) return messages;
  json encoded = messages;
  for (json& message : encoded) {
    if (message.is_object()) {
      message.erase(kWireReplayField);
      message.erase(kReadRangeField);
    }
  }
  return encoded;
}

json FunctionTools(WireApi wire_api, const json& schemas,
                   bool skip_web_search = false) {
  json tools = json::array();
  if (!schemas.is_array()) return tools;
  for (const json& schema : schemas) {
    const json* function = JsonObject(schema, "function");
    if (!function ||
        (skip_web_search && JsonValue(*function, "name", "") == "web_search")) {
      continue;
    }
    if (wire_api == WireApi::kChatCompletions) {
      tools.push_back(schema);
      continue;
    }
    json tool;
    if (wire_api == WireApi::kResponses) {
      tool = *function;
      tool["type"] = "function";
    } else {
      tool = {
          {"name", JsonValue(*function, "name", "")},
          {"input_schema", JsonValue(*function, "parameters", json::object())}};
      if (function->contains("description")) {
        tool["description"] = (*function)["description"];
      }
      if (function->contains("strict")) tool["strict"] = (*function)["strict"];
    }
    if (!JsonValue(tool, "name", "").empty()) tools.push_back(std::move(tool));
  }
  return tools;
}

json ResponsesContent(const json& content, bool assistant) {
  json blocks = json::array();
  auto text = [&](const std::string& value) {
    blocks.push_back(
        {{"type", assistant ? "output_text" : "input_text"}, {"text", value}});
  };
  if (content.is_string()) {
    text(content.get<std::string>());
    return blocks;
  }
  if (!content.is_array()) return blocks;
  for (const json& part : content) {
    const std::string type = JsonValue(part, "type", "");
    if (type == "text") {
      text(JsonValue(part, "text", ""));
    } else if (type == "image_url") {
      const json* image = JsonObject(part, "image_url");
      if (!image) continue;
      json block = {{"type", "input_image"},
                    {"image_url", JsonValue(*image, "url", "")}};
      if (image->contains("detail")) block["detail"] = (*image)["detail"];
      if (!JsonValue(block, "image_url", "").empty()) {
        blocks.push_back(std::move(block));
      }
    } else if (type == "file") {
      const json* file = JsonObject(part, "file");
      if (!file) continue;
      json block = {{"type", "input_file"}};
      if (file->contains("filename")) block["filename"] = (*file)["filename"];
      if (file->contains("file_data")) {
        block["file_data"] = (*file)["file_data"];
      }
      if (block.size() > 1) blocks.push_back(std::move(block));
    }
  }
  return blocks;
}

void AppendResponsesMessage(json& input, const json& message) {
  const std::string role = JsonValue(message, "role", "");
  if (role == "tool") {
    input.push_back({{"type", "function_call_output"},
                     {"call_id", JsonValue(message, "tool_call_id", "")},
                     {"output", JsonValue(message, "content", "")}});
    return;
  }
  if (role != "system" && role != "developer" && role != "user" &&
      role != "assistant") {
    return;
  }
  bool replay_message = false;
  std::set<std::string> replay_calls;
  if (role == "assistant") {
    if (const json* replay = MatchingReplay(message, "responses")) {
      const json* items = JsonArray(*replay, "items");
      if (items) {
        for (const json& item : *items) {
          input.push_back(item);
          const std::string type = JsonValue(item, "type", "");
          replay_message = replay_message || type == "message";
          if (type == "function_call") {
            replay_calls.insert(JsonValue(item, "call_id", ""));
          }
        }
      }
    }
  }
  json content = ResponsesContent(
      message.contains("content") ? message["content"] : json(nullptr),
      role == "assistant");
  if (!content.empty() && !replay_message) {
    input.push_back(
        {{"type", "message"}, {"role", role}, {"content", std::move(content)}});
  }
  if (role != "assistant") return;
  const json* calls = JsonArray(message, "tool_calls");
  if (!calls) return;
  for (const json& call : *calls) {
    const std::string call_id = JsonValue(call, "id", "");
    if (replay_calls.contains(call_id)) continue;
    const json* function = JsonObject(call, "function");
    if (!function) continue;
    input.push_back({{"type", "function_call"},
                     {"call_id", call_id},
                     {"name", JsonValue(*function, "name", "")},
                     {"arguments", JsonValue(*function, "arguments", "")}});
  }
}

json ResponsesInput(const json& messages) {
  if (!messages.is_array()) return messages;
  json input = json::array();
  for (const json& message : messages) {
    if (message.is_object()) AppendResponsesMessage(input, message);
  }
  return input;
}

bool DataUri(std::string_view uri, std::string& media_type, std::string& data) {
  constexpr std::string_view kMarker = ";base64,";
  if (!uri.starts_with("data:")) return false;
  size_t split = uri.find(kMarker, 5);
  if (split == std::string_view::npos || split == 5) return false;
  media_type = std::string(uri.substr(5, split - 5));
  data = std::string(uri.substr(split + kMarker.size()));
  return !data.empty();
}

json AnthropicContent(const json& content) {
  json blocks = json::array();
  if (content.is_string()) {
    blocks.push_back({{"type", "text"}, {"text", content}});
    return blocks;
  }
  if (!content.is_array()) return blocks;
  for (const json& part : content) {
    const std::string type = JsonValue(part, "type", "");
    if (type == "text") {
      blocks.push_back(
          {{"type", "text"}, {"text", JsonValue(part, "text", "")}});
      continue;
    }
    if (type == "image_url") {
      const json* image = JsonObject(part, "image_url");
      if (!image) continue;
      std::string url = JsonValue(*image, "url", "");
      std::string media_type;
      std::string data;
      json source;
      if (DataUri(url, media_type, data)) {
        source = {{"type", "base64"},
                  {"media_type", std::move(media_type)},
                  {"data", std::move(data)}};
      } else if (!url.empty()) {
        source = {{"type", "url"}, {"url", std::move(url)}};
      }
      if (!source.empty()) {
        blocks.push_back({{"type", "image"}, {"source", std::move(source)}});
      }
      continue;
    }
    if (type != "file") continue;
    const json* file = JsonObject(part, "file");
    if (!file) continue;
    std::string media_type;
    std::string data;
    if (!DataUri(JsonValue(*file, "file_data", ""), media_type, data)) {
      continue;
    }
    json block = {{"type", "document"},
                  {"source",
                   {{"type", "base64"},
                    {"media_type", std::move(media_type)},
                    {"data", std::move(data)}}}};
    if (file->contains("filename")) block["title"] = (*file)["filename"];
    blocks.push_back(std::move(block));
  }
  return blocks;
}

void AppendAnthropicMessage(json& messages, std::string role, json blocks) {
  if (blocks.empty()) return;
  if (!messages.empty() && JsonValue(messages.back(), "role", "") == role) {
    json& content = messages.back()["content"];
    for (json& block : blocks) content.push_back(std::move(block));
    return;
  }
  messages.push_back(
      {{"role", std::move(role)}, {"content", std::move(blocks)}});
}

std::string SystemText(const json& content) {
  if (content.is_string()) return content.get<std::string>();
  if (!content.is_array()) return "";
  std::string result;
  for (const json& part : content) {
    if (JsonValue(part, "type", "") != "text") continue;
    std::string text = JsonValue(part, "text", "");
    if (!text.empty()) result += (result.empty() ? "" : "\n\n") + text;
  }
  return result;
}

json AnthropicMessages(const json& canonical, std::string& system) {
  if (!canonical.is_array()) return canonical;
  json messages = json::array();
  for (const json& message : canonical) {
    if (!message.is_object()) continue;
    const std::string role = JsonValue(message, "role", "");
    const json& content =
        message.contains("content") ? message["content"] : json(nullptr);
    if (role == "system" || role == "developer") {
      std::string text = SystemText(content);
      if (!text.empty()) system += (system.empty() ? "" : "\n\n") + text;
      continue;
    }
    if (role == "tool") {
      json blocks =
          json::array({{{"type", "tool_result"},
                        {"tool_use_id", JsonValue(message, "tool_call_id", "")},
                        {"content", JsonValue(message, "content", "")}}});
      AppendAnthropicMessage(messages, "user", std::move(blocks));
      continue;
    }
    if (role != "user" && role != "assistant") continue;
    json blocks;
    if (role == "assistant") {
      const json* replay = MatchingReplay(message, "anthropic_messages");
      const json* replay_content =
          replay ? JsonArray(*replay, "content") : nullptr;
      if (replay_content && !replay_content->empty()) {
        blocks = *replay_content;
      } else {
        blocks = AnthropicContent(content);
        const json* calls = JsonArray(message, "tool_calls");
        if (calls) {
          for (const json& call : *calls) {
            const json* function = JsonObject(call, "function");
            if (!function) continue;
            json input = json::parse(JsonValue(*function, "arguments", ""),
                                     nullptr, false);
            if (input.is_discarded() || !input.is_object()) {
              input = json::object();
            }
            blocks.push_back({{"type", "tool_use"},
                              {"id", JsonValue(call, "id", "")},
                              {"name", JsonValue(*function, "name", "")},
                              {"input", std::move(input)}});
          }
        }
      }
    } else {
      blocks = AnthropicContent(content);
    }
    AppendAnthropicMessage(messages, role, std::move(blocks));
  }
  return messages;
}

json RequestTools(WireApi wire_api, const WireRequest& request) {
  json tools = request.native_tools
                   ? FunctionTools(wire_api, request.tool_schemas,
                                   (wire_api != WireApi::kChatCompletions &&
                                    request.native_web_search) ||
                                       !request.function_web_search)
                   : json::array();
  if (request.native_web_search) {
    if (wire_api == WireApi::kResponses) {
      tools.push_back({{"type", "web_search"}});
    } else if (wire_api == WireApi::kAnthropicMessages) {
      tools.push_back(
          {{"type", "web_search_20250305"}, {"name", "web_search"}});
    }
  }
  return tools;
}

json RequestEnvelope(WireApi wire_api, const WireRequest& request, json tools) {
  json body = {{"model", request.model}, {"stream", true}};
  if (!tools.empty()) {
    body["tools"] = std::move(tools);
    if (request.parallel_tools && wire_api != WireApi::kAnthropicMessages) {
      body["parallel_tool_calls"] = true;
    }
  }
  if (wire_api == WireApi::kChatCompletions) {
    if (request.stream_usage) {
      body["stream_options"] = {{"include_usage", true}};
    }
  } else if (wire_api == WireApi::kResponses) {
    body["store"] = false;
    json include = json::array({"reasoning.encrypted_content"});
    if (request.native_web_search && request.include_web_search_sources) {
      include.push_back("web_search_call.action.sources");
    }
    body["include"] = std::move(include);
    if (request.max_output_tokens > 0) {
      body["max_output_tokens"] = request.max_output_tokens;
    }
    if (!request.reasoning_effort.empty()) {
      body["reasoning"] = {{"effort", request.reasoning_effort}};
    }
  } else {
    body["cache_control"] = {{"type", "ephemeral"}};
    body["max_tokens"] = request.max_output_tokens > 0
                             ? request.max_output_tokens
                             : int64_t{8192};
    if (request.reasoning_effort == "low" ||
        request.reasoning_effort == "medium" ||
        request.reasoning_effort == "high" ||
        request.reasoning_effort == "max") {
      body["thinking"] = {{"type", "adaptive"}};
      body["output_config"] = {{"effort", request.reasoning_effort}};
    }
  }
  return body;
}

constexpr char kEncodedSlot[] = "\x01uagent-encoded\x01";

std::string ArrayContents(const json& array) {
  std::string text = JsonDump(array);
  return text.substr(1, text.size() - 2);
}

}  // namespace

bool WireSupportsHostedTool(WireApi wire_api, HostedTool tool) {
  return tool == HostedTool::kWebSearch &&
         (wire_api == WireApi::kResponses ||
          wire_api == WireApi::kAnthropicMessages);
}

json EncodeWireRequest(WireApi wire_api, const WireRequest& request) {
  json body =
      RequestEnvelope(wire_api, request, RequestTools(wire_api, request));
  std::string system;
  switch (wire_api) {
    case WireApi::kChatCompletions:
      body["messages"] = ChatMessages(request.messages);
      break;
    case WireApi::kResponses:
      body["input"] = ResponsesInput(request.messages);
      break;
    case WireApi::kAnthropicMessages:
      body["messages"] = AnthropicMessages(request.messages, system);
      if (!system.empty()) body["system"] = std::move(system);
      break;
  }
  return body;
}

json WireRequestCache::Encode(WireApi wire_api, const WireRequest& request) {
  active_ = request.messages.is_array();
  if (!active_) return EncodeWireRequest(wire_api, request);
  if (wire_api_ != wire_api) {
    messages_.clear();
    tool_key_ = nullptr;
    wire_api_ = wire_api;
  }
  messages_.resize(request.messages.size());
  std::string system;
  std::string role;
  encoded_messages_ = "[";
  for (size_t index = 0; index < messages_.size(); ++index) {
    Message& entry = messages_[index];
    const json& source = request.messages[index];
    if (!entry.valid || entry.source != source) {
      entry = {source, {}, {}, {}, true};
      json single = json::array({source});
      if (wire_api == WireApi::kAnthropicMessages) {
        json encoded = AnthropicMessages(single, entry.system);
        if (!encoded.empty()) {
          entry.role = JsonValue(encoded[0], "role", "");
          entry.body = ArrayContents(encoded[0]["content"]);
        }
      } else {
        entry.body = ArrayContents(wire_api == WireApi::kResponses
                                       ? ResponsesInput(single)
                                       : ChatMessages(single));
      }
    }
    if (!entry.system.empty()) {
      if (!system.empty()) system += "\n\n";
      system += entry.system;
    }
    if (entry.body.empty()) continue;
    if (wire_api == WireApi::kAnthropicMessages) {
      if (role != entry.role) {
        if (!role.empty()) {
          encoded_messages_ += "],\"role\":" + JsonDump(role) + "},";
        }
        encoded_messages_ += "{\"content\":[";
        role = entry.role;
      } else {
        encoded_messages_ += ',';
      }
    } else if (encoded_messages_.size() > 1) {
      encoded_messages_ += ',';
    }
    encoded_messages_ += entry.body;
  }
  if (!role.empty()) encoded_messages_ += "],\"role\":" + JsonDump(role) + "}";
  encoded_messages_ += ']';

  json key = json::array({request.native_tools, request.native_web_search,
                          request.function_web_search});
  if (key != tool_key_ || schemas_ != request.tool_schemas) {
    encoded_tools_ = JsonDump(RequestTools(wire_api, request));
    tool_key_ = std::move(key);
    schemas_ = request.tool_schemas;
  }
  json body = RequestEnvelope(
      wire_api, request,
      encoded_tools_ == "[]" ? json::array() : json(kEncodedSlot));
  body[wire_api == WireApi::kResponses ? "input" : "messages"] = kEncodedSlot;
  if (!system.empty()) body["system"] = std::move(system);
  return body;
}

std::string WireRequestCache::Serialize(const json& body) const {
  std::string payload = JsonDump(body);
  if (!active_) return payload;
  for (const char* field : {"messages", "input", "tools"}) {
    if (JsonValue(body, field, "") != kEncodedSlot) continue;
    std::string key = JsonDump(field) + ":";
    std::string marker = key + JsonDump(kEncodedSlot);
    size_t at = payload.find(marker);
    if (at != std::string::npos) {
      payload.replace(at + key.size(), marker.size() - key.size(),
                      std::string_view(field) == "tools" ? encoded_tools_
                                                         : encoded_messages_);
    }
  }
  return payload;
}

}  // namespace uagent
