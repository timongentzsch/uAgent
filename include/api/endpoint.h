// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_ENDPOINT_H_
#define UAGENT_INCLUDE_API_ENDPOINT_H_
// The two records every route is made of: where requests go and what that
// place does (Endpoint), and what is asked there (Route).

#include <cstdint>
#include <string>
#include <vector>

#include "include/api/capabilities.h"
#include "include/core/json.h"

namespace uagent {

struct Endpoint {
  // Empty for an endpoint no provider entry or known table row names.
  std::string name, base_url, api_key;
  WireApi wire_api = WireApi::kChatCompletions;
  // The window the endpoint states where its catalogue reports none.
  int64_t context = 0;
  FeatureSet features = {};
  // Request extras the features read: `provider` (the upstream a
  // provider_routing endpoint is pinned to) and `pdf_engine`.
  json options = nullptr;
  // What the configuration declares of the models there; the catalogue's are
  // merged into the capabilities when a route is applied.
  json model_features = nullptr;

  std::string Option(const char* key) const {
    return JsonValue(options, key, "");
  }
};

// An endpoint, the model asked there and how hard it thinks.
struct Route : Endpoint {
  std::string model, reasoning_effort;
  // The selection a catalogue route was chosen by and the model it named,
  // so the choice can be written down as it was made (an alias stays one).
  std::string route_name, route_model;
  std::vector<std::string> supported_reasoning_efforts;
  int64_t ctx_window = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_ENDPOINT_H_
