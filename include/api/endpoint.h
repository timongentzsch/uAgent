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
  // Request extras the features read: `provider` and `pdf_engine`.
  json options = nullptr;
  // Model features the configuration declares; a route adds the catalogue's.
  json model_features = nullptr;
  // Tools the endpoint runs itself, each as its API takes it.
  json hosted_tools = json::array();

  // A free-only OpenRouter preset never enables billable hosted extras.
  bool FreeOnly() const { return JsonValue(options, "free_only", false); }

  std::string Option(const char* key) const {
    return JsonValue(options, key, "");
  }
};

struct Route : Endpoint {
  std::string model, reasoning_effort;
  // The selection the route was chosen by and the model it named.
  std::string route_name, route_model;
  std::vector<std::string> supported_reasoning_efforts;
  int64_t ctx_window = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_ENDPOINT_H_
