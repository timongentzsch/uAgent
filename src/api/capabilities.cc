// Copyright 2026 Timon Gentzsch

#include "include/api/capabilities.h"

#include <algorithm>
#include <string>
#include <utility>

#include "include/core/strings.h"

namespace uagent {

const char* ProviderProtocolName(ProviderProtocol protocol) {
  switch (protocol) {
    case ProviderProtocol::kOpenAi:
      return "openai";
    case ProviderProtocol::kOpenRouter:
      return "openrouter";
    case ProviderProtocol::kAnthropic:
      return "anthropic";
  }
  return "openai";
}

std::optional<ProviderProtocol> ParseProviderProtocol(
    std::string_view protocol) {
  if (protocol == "openai") return ProviderProtocol::kOpenAi;
  if (protocol == "openrouter") return ProviderProtocol::kOpenRouter;
  if (protocol == "anthropic") return ProviderProtocol::kAnthropic;
  return std::nullopt;
}

const char* WireApiName(WireApi wire_api) {
  switch (wire_api) {
    case WireApi::kChatCompletions:
      return "chat_completions";
    case WireApi::kResponses:
      return "responses";
    case WireApi::kAnthropicMessages:
      return "anthropic_messages";
  }
  return "chat_completions";
}

std::optional<WireApi> ParseWireApi(std::string_view wire_api) {
  if (wire_api == "chat_completions") return WireApi::kChatCompletions;
  if (wire_api == "responses") return WireApi::kResponses;
  if (wire_api == "anthropic_messages") return WireApi::kAnthropicMessages;
  return std::nullopt;
}

bool HasHostedTool(const json& hosted_tools, HostedTool tool) {
  if (!hosted_tools.is_array()) return false;
  const std::string_view wanted =
      tool == HostedTool::kWebSearch ? "web_search" : "";
  for (const json& value : hosted_tools) {
    if (value.is_string() && value.get_ref<const std::string&>() == wanted) {
      return true;
    }
  }
  return false;
}

json HostedToolsJson(bool web_search) {
  json tools = json::array();
  if (web_search) tools.push_back("web_search");
  return tools;
}

void ProviderCapabilities::SetInputModalities(const json& modalities) {
  if (!modalities.is_array() || modalities == input_modalities) return;
  input_modalities = modalities;
  image_input = std::find(modalities.begin(), modalities.end(), "image") !=
                modalities.end();
  file_input = std::find(modalities.begin(), modalities.end(), "pdf") !=
                   modalities.end() ||
               std::find(modalities.begin(), modalities.end(), "file") !=
                   modalities.end();
  // Only the Chat Completions dialect has speech and video parts; Responses
  // and Anthropic Messages accept text, images and files.
  const bool chat = wire_api == WireApi::kChatCompletions;
  audio_input = chat && std::find(modalities.begin(), modalities.end(),
                                  "audio") != modalities.end();
  video_input = chat && std::find(modalities.begin(), modalities.end(),
                                  "video") != modalities.end();
}

void ProviderCapabilities::SetModelFeatures(const json& features) {
  if (!features.is_object()) return;
  json merged = features;
  // Explicit route features and a negotiated rejection survive catalog
  // refreshes. Changing route creates a fresh capability object.
  if (model_features.is_object()) merged.update(model_features);
  model_features = std::move(merged);
  reasoning_summary =
      JsonValue(model_features, "reasoning_summary", reasoning_summary);
  adaptive_thinking =
      JsonValue(model_features, "adaptive_thinking", adaptive_thinking);
}

void ProviderCapabilities::ResetNegotiated() {
  native_tools = true;
  parallel_tools = true;
  stream_usage_option = wire_api == WireApi::kChatCompletions && !OpenRouter();
  image_input = true;
  file_input = true;
  audio_input = wire_api == WireApi::kChatCompletions;
  video_input = audio_input;
  json modalities = std::move(input_modalities);
  input_modalities = nullptr;
  SetInputModalities(modalities);
  reasoning_text = false;
  reasoning_details = false;
  reasoning_content = false;
  citations = false;
  reported_usage = false;
}

void ProviderCapabilities::Observe(const ChatResult& result) {
  reasoning_text = reasoning_text || result.reasoning_field;
  reasoning_details = reasoning_details || result.reasoning_details_field;
  reasoning_content = reasoning_content || result.reasoning_content_field;
  citations = citations || !result.annotations.empty();
  reported_usage =
      reported_usage || (result.usage.is_object() && !result.usage.empty());
}

json ProviderCapabilities::DiagnosticJson() const {
  return {{"protocol", ProviderProtocolName(protocol)},
          {"wire_api", WireApiName(wire_api)},
          {"hosted_tools", HostedToolsJson(hosted_web_search)},
          {"native_tools", native_tools},
          {"parallel_tools", parallel_tools},
          {"stream_usage_option", stream_usage_option},
          {"reasoning_summary", reasoning_summary},
          {"adaptive_thinking", adaptive_thinking},
          {"image_input", image_input},
          {"file_input", file_input},
          {"audio_input", audio_input},
          {"video_input", video_input},
          {"input_modalities", input_modalities},
          {"web_search_sources", web_search_sources},
          {"model_catalog_required", model_catalog_required},
          {"raw_slash_models", raw_slash_models},
          {"reasoning_object", reasoning_object},
          {"reasoning_replay_text", reasoning_replay_text},
          {"max_completion_tokens", max_completion_tokens},
          {"provider_routing", provider_routing},
          {"session_passthrough", session_passthrough},
          {"model_variants", model_variants},
          {"observed_reasoning_text", reasoning_text},
          {"observed_reasoning_details", reasoning_details},
          {"observed_reasoning_content", reasoning_content},
          {"observed_citations", citations},
          {"observed_usage", reported_usage}};
}

ProviderCapabilities CapabilitiesForRoute(ProviderProtocol protocol,
                                          const std::string& base_url,
                                          WireApi wire_api,
                                          bool hosted_web_search) {
  ProviderCapabilities capabilities;
  capabilities.protocol = protocol;
  capabilities.wire_api = wire_api;
  capabilities.hosted_web_search = hosted_web_search;
  if (protocol == ProviderProtocol::kOpenRouter) {
    capabilities.model_catalog_required = false;
    capabilities.raw_slash_models = true;
    capabilities.reasoning_object = true;
    capabilities.reasoning_replay_text = true;
    capabilities.provider_routing = true;
    capabilities.session_passthrough = true;
    capabilities.model_variants = true;
  } else if (protocol == ProviderProtocol::kAnthropic) {
    capabilities.reasoning_object = true;
  } else if (OpenaiUrl(base_url)) {
    // Exact official host, not a model-name or substring heuristic: OpenAI's
    // Chat Completions dialect uses max_completion_tokens.
    capabilities.max_completion_tokens = true;
  }
  if (wire_api == WireApi::kResponses) {
    capabilities.reasoning_object = true;
    capabilities.max_completion_tokens = true;
    capabilities.web_search_sources = OpenaiUrl(base_url);
    capabilities.reasoning_summary = OpenaiUrl(base_url);
  }
  capabilities.ResetNegotiated();
  return capabilities;
}

RejectedCapability RejectedRouteCapability(
    const ChatResult& result, const ProviderCapabilities& capabilities) {
  std::string structured =
      AsciiLower(result.remote_error_type + " " + result.remote_error_code);
  std::string message = AsciiLower(result.error);
  std::string evidence = structured + " " + message;
  if (result.http_status != 400 && result.http_status != 422) {
    return RejectedCapability::kNone;
  }
  auto unsupported_modality = [&](std::string_view noun) {
    const bool explicit_rejection =
        structured.find("unsupported") != std::string::npos ||
        evidence.find("does not support") != std::string::npos ||
        evidence.find("not supported") != std::string::npos ||
        evidence.find("unsupported input") != std::string::npos;
    const bool invalid_file =
        evidence.find("size") != std::string::npos ||
        evidence.find("too large") != std::string::npos ||
        evidence.find("corrupt") != std::string::npos ||
        message.find("invalid image") != std::string::npos ||
        message.find("invalid file") != std::string::npos ||
        message.find("invalid pdf") != std::string::npos ||
        evidence.find("format") != std::string::npos ||
        evidence.find("dimension") != std::string::npos;
    return explicit_rejection && !invalid_file &&
           evidence.find(noun) != std::string::npos;
  };
  if (capabilities.image_input && unsupported_modality("image")) {
    return RejectedCapability::kImageInput;
  }
  if (capabilities.file_input &&
      (unsupported_modality("file") || unsupported_modality("document") ||
       unsupported_modality("pdf"))) {
    return RejectedCapability::kFileInput;
  }
  if (capabilities.audio_input && unsupported_modality("audio")) {
    return RejectedCapability::kAudioInput;
  }
  if (capabilities.video_input && unsupported_modality("video")) {
    return RejectedCapability::kVideoInput;
  }
  if (result.http_status != 400) return RejectedCapability::kNone;
  if (capabilities.reasoning_summary &&
      (evidence.find("reasoning.summary") != std::string::npos ||
       evidence.find("'reasoning'") != std::string::npos ||
       evidence.find("\"reasoning\"") != std::string::npos ||
       evidence.find("thinking.display") != std::string::npos) &&
      (evidence.find("unsupported") != std::string::npos ||
       evidence.find("not supported") != std::string::npos ||
       evidence.find("unknown") != std::string::npos)) {
    return RejectedCapability::kReasoningSummary;
  }
  if (capabilities.parallel_tools &&
      (evidence.find("parallel_tool_calls") != std::string::npos ||
       evidence.find("parallel tool calls") != std::string::npos)) {
    return RejectedCapability::kParallelTools;
  }
  if (capabilities.stream_usage_option &&
      evidence.find("stream_options") != std::string::npos) {
    return RejectedCapability::kStreamUsage;
  }
  return RejectedCapability::kNone;
}

const char* CapabilityName(RejectedCapability capability) {
  switch (capability) {
    case RejectedCapability::kImageInput:
      return "image_input";
    case RejectedCapability::kFileInput:
      return "file_input";
    case RejectedCapability::kAudioInput:
      return "audio_input";
    case RejectedCapability::kVideoInput:
      return "video_input";
    case RejectedCapability::kParallelTools:
      return "parallel_tool_calls";
    case RejectedCapability::kStreamUsage:
      return "stream_options";
    case RejectedCapability::kReasoningSummary:
      return "reasoning_summary";
    case RejectedCapability::kNone:
      return "none";
  }
  return "none";
}

}  // namespace uagent
