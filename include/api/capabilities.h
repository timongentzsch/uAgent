// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_CAPABILITIES_H_
#define UAGENT_INCLUDE_API_CAPABILITIES_H_
// Route behavior is declared once here instead of inferred repeatedly from a
// provider name, model name, or response rendering shape.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "include/api/types.h"
#include "include/core/json.h"

namespace uagent {

enum class ProviderProtocol : uint8_t { kOpenAi, kOpenRouter, kAnthropic };
enum class WireApi : uint8_t {
  kChatCompletions,
  kResponses,
  kAnthropicMessages,
};
enum class HostedTool : uint8_t { kWebSearch };
enum class RejectedCapability : uint8_t {
  kNone,
  kImageInput,
  kFileInput,
  kParallelTools,
  kStreamUsage,
};

const char* ProviderProtocolName(ProviderProtocol protocol);
std::optional<ProviderProtocol> ParseProviderProtocol(
    std::string_view protocol);
const char* WireApiName(WireApi wire_api);
std::optional<WireApi> ParseWireApi(std::string_view wire_api);
bool HasHostedTool(const json& hosted_tools, HostedTool tool);
json HostedToolsJson(bool web_search);

struct ProviderCapabilities {
  ProviderProtocol protocol = ProviderProtocol::kOpenAi;
  WireApi wire_api = WireApi::kChatCompletions;
  bool hosted_web_search = false;

  // Request features that may be downgraded after a structured rejection.
  bool native_tools = true;
  bool parallel_tools = true;
  bool stream_usage_option = true;
  bool image_input = true;
  // Document parts, which not every route accepts even when it takes images.
  bool file_input = true;

  // Stable route dialect features.
  bool model_catalog_required = true;
  bool raw_slash_models = false;
  bool reasoning_object = false;
  bool reasoning_replay_text = false;
  bool max_completion_tokens = false;
  // Optional Responses expansion supported by the official OpenAI endpoint.
  // Compatible routes can still run hosted search without returning its full
  // internal source list.
  bool web_search_sources = false;
  bool provider_routing = false;
  bool session_passthrough = false;
  bool model_variants = false;

  // Features observed in successful responses. They are diagnostic facts, not
  // prerequisites and never drive the current turn.
  bool reasoning_text = false;
  bool reasoning_details = false;
  bool reasoning_content = false;
  bool citations = false;
  bool reported_usage = false;

  bool OpenRouter() const { return protocol == ProviderProtocol::kOpenRouter; }
  bool Anthropic() const { return protocol == ProviderProtocol::kAnthropic; }
  bool Supports(HostedTool tool) const {
    return tool == HostedTool::kWebSearch && hosted_web_search;
  }
  void ResetNegotiated();
  void Observe(const ChatResult& result);
  json DiagnosticJson() const;
};

ProviderCapabilities CapabilitiesForRoute(
    ProviderProtocol protocol, const std::string& base_url,
    WireApi wire_api = WireApi::kChatCompletions,
    bool hosted_web_search = false);
RejectedCapability RejectedRouteCapability(
    const ChatResult& result, const ProviderCapabilities& capabilities);
const char* CapabilityName(RejectedCapability capability);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_CAPABILITIES_H_
