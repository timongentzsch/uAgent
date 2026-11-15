// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_CAPABILITIES_H_
#define UAGENT_INCLUDE_API_CAPABILITIES_H_
// Route behavior is declared here, not inferred from provider or model names.

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "include/api/types.h"
#include "include/core/json.h"

namespace uagent {

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
  kAudioInput,
  kVideoInput,
  kParallelTools,
  kStreamUsage,
  kReasoningSummary,
};

// What an endpoint declares beyond the wire API it speaks.
enum class Feature : uint8_t {
  kHostedSearch,         // the endpoint runs a search for web_search
  kProviderRouting,      // options.provider pins an upstream; requests may
                         // carry provider preferences (privacy, price)
  kSessionPassthrough,   // the request carries the session id
  kRawVendorIds,         // `vendor/model` ids are served without an alias
  kModelVariants,        // an id may end in a routing suffix (`:nitro`)
  kReasoningObject,      // effort is {"reasoning": {"effort": ...}}
  kReasoningReplayText,  // reasoning text is sent back as `reasoning`
  kPromptCacheControl,   // a top-level cache_control asks for prompt caching
  kPdfParser,            // options.pdf_engine reads documents
  kMaxCompletionTokens,  // the output limit is max_completion_tokens
  kReasoningSummary,     // Responses can summarize reasoning
  kInlineUsage,          // usage arrives without asking for it in the stream
  kDecisions,            // options.decisions_url serves decision models
  kCount,
};

struct FeatureSet {
  uint32_t bits = 0;

  constexpr bool Has(Feature feature) const {
    return ((bits >> static_cast<uint8_t>(feature)) & 1U) != 0;
  }
  constexpr void Add(Feature feature) {
    bits |= uint32_t{1} << static_cast<uint8_t>(feature);
  }
  constexpr void Remove(Feature feature) {
    bits &= ~(uint32_t{1} << static_cast<uint8_t>(feature));
  }
  bool operator==(const FeatureSet&) const = default;
};

constexpr FeatureSet FeaturesOf(std::initializer_list<Feature> features) {
  FeatureSet set;
  for (Feature feature : features) set.Add(feature);
  return set;
}

// The names of a set, in declaration order.
json FeatureNames(const FeatureSet& features);
// An array of names as a set; false names the first that is none.
bool ParseFeatures(const json& names, FeatureSet& features,
                   std::string& unknown);

const char* WireApiName(WireApi wire_api);
std::optional<WireApi> ParseWireApi(std::string_view wire_api);
bool HasHostedTool(const json& hosted_tools, HostedTool tool);
// The entries of a `hosted_tools` list that are objects with a `type`.
json HostedToolDefinitions(const json& hosted_tools);

// What a route negotiated or learned beyond what its endpoint declares.
struct ProviderCapabilities {
  // Request features that may be downgraded after a structured rejection.
  bool parallel_tools = true;
  bool stream_usage_option = true;
  bool reasoning_summary = false;
  bool adaptive_thinking = false;
  json model_features = nullptr;
  json input_modalities = nullptr;
  bool image_input = true;
  // Document parts, which not every route accepts even when it takes images.
  bool file_input = true;
  bool audio_input = true;
  bool video_input = true;

  // Observed in successful responses: diagnostic only, never drives a turn.
  bool reasoning_text = false;
  bool reasoning_details = false;
  bool reasoning_content = false;
  bool citations = false;
  bool reported_usage = false;

  // Only Chat Completions has speech and video parts.
  void SetInputModalities(const json& modalities, WireApi wire_api);
  void SetModelFeatures(const json& features);
  void ResetNegotiated(WireApi wire_api, const FeatureSet& declared);
  void Observe(const ChatResult& result);
  json DiagnosticJson(WireApi wire_api, const FeatureSet& declared) const;
};

ProviderCapabilities CapabilitiesForRoute(WireApi wire_api,
                                          const FeatureSet& declared);
RejectedCapability RejectedRouteCapability(
    const ChatResult& result, const ProviderCapabilities& capabilities);
const char* CapabilityName(RejectedCapability capability);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_CAPABILITIES_H_
