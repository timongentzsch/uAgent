// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_CAPABILITIES_H_
#define UAGENT_INCLUDE_API_CAPABILITIES_H_
// Route behavior is declared once here instead of inferred repeatedly from a
// provider name, model name, or response rendering shape.

#include <cstdint>
#include <string>

#include "include/api/types.h"
#include "include/core/json.h"

namespace uagent {

enum class ProviderProtocol : uint8_t { kOpenAi, kOpenRouter };
enum class RejectedCapability : uint8_t {
  kNone,
  kImageInput,
  kParallelTools,
  kStreamUsage,
  kNativeTools,
};

const char* ProviderProtocolName(ProviderProtocol protocol);
ProviderProtocol ParseProviderProtocol(const std::string& protocol);

struct ProviderCapabilities {
  ProviderProtocol protocol = ProviderProtocol::kOpenAi;

  // Request features that may be downgraded after a structured rejection.
  bool native_tools = true;
  bool parallel_tools = true;
  bool stream_usage_option = true;
  bool image_input = true;

  // Stable route dialect features.
  bool model_catalog_required = true;
  bool raw_slash_models = false;
  bool reasoning_object = false;
  bool reasoning_replay_text = false;
  bool max_completion_tokens = false;
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
  void ResetNegotiated();
  void Observe(const ChatResult& result);
  json DiagnosticJson() const;
};

ProviderCapabilities CapabilitiesForRoute(ProviderProtocol protocol,
                                          const std::string& base_url);
RejectedCapability RejectedRouteCapability(
    const ChatResult& result, const ProviderCapabilities& capabilities);
const char* CapabilityName(RejectedCapability capability);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_CAPABILITIES_H_
