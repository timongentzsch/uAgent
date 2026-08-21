// Copyright 2026 Timon Gentzsch

#include "include/api/capabilities.h"

#include <string>

#include "include/core/strings.h"

namespace uagent {

const char* ProviderProtocolName(ProviderProtocol protocol) {
  return protocol == ProviderProtocol::kOpenRouter ? "openrouter" : "openai";
}

ProviderProtocol ParseProviderProtocol(const std::string& protocol) {
  return protocol == "openrouter" ? ProviderProtocol::kOpenRouter
                                  : ProviderProtocol::kOpenAi;
}

void ProviderCapabilities::ResetNegotiated() {
  native_tools = true;
  parallel_tools = true;
  stream_usage_option = !OpenRouter();
  image_input = true;
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
          {"native_tools", native_tools},
          {"parallel_tools", parallel_tools},
          {"stream_usage_option", stream_usage_option},
          {"image_input", image_input},
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
                                          const std::string& base_url) {
  ProviderCapabilities capabilities;
  capabilities.protocol = protocol;
  if (protocol == ProviderProtocol::kOpenRouter) {
    capabilities.model_catalog_required = false;
    capabilities.raw_slash_models = true;
    capabilities.reasoning_object = true;
    capabilities.reasoning_replay_text = true;
    capabilities.provider_routing = true;
    capabilities.session_passthrough = true;
    capabilities.model_variants = true;
  } else if (OpenaiUrl(base_url)) {
    capabilities.max_completion_tokens = true;
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
  auto unsupported_modality = [&](std::string_view noun) {
    return evidence.find(noun) != std::string::npos &&
           (evidence.find("input") != std::string::npos ||
            evidence.find("support") != std::string::npos ||
            evidence.find("modalit") != std::string::npos);
  };
  if (capabilities.image_input && unsupported_modality("image")) {
    return RejectedCapability::kImageInput;
  }
  if (capabilities.file_input &&
      (unsupported_modality("file") || unsupported_modality("document") ||
       unsupported_modality("pdf"))) {
    return RejectedCapability::kFileInput;
  }
  if (result.http_status != 400) return RejectedCapability::kNone;
  if (capabilities.parallel_tools &&
      (evidence.find("parallel_tool_calls") != std::string::npos ||
       evidence.find("parallel tool calls") != std::string::npos)) {
    return RejectedCapability::kParallelTools;
  }
  if (capabilities.stream_usage_option &&
      evidence.find("stream_options") != std::string::npos) {
    return RejectedCapability::kStreamUsage;
  }
  if (capabilities.native_tools && evidence.find("tool") != std::string::npos) {
    return RejectedCapability::kNativeTools;
  }
  return RejectedCapability::kNone;
}

const char* CapabilityName(RejectedCapability capability) {
  switch (capability) {
    case RejectedCapability::kImageInput:
      return "image_input";
    case RejectedCapability::kFileInput:
      return "file_input";
    case RejectedCapability::kParallelTools:
      return "parallel_tool_calls";
    case RejectedCapability::kStreamUsage:
      return "stream_options";
    case RejectedCapability::kNativeTools:
      return "native_tools";
    case RejectedCapability::kNone:
      return "none";
  }
  return "none";
}

}  // namespace uagent
