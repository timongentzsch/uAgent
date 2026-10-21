// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
void ApplySelectionPolicy(Api& api, const ModelSelection& selection) {
  if (!selection.effort.empty() &&
      SupportsReasoningEffort(api, selection.effort)) {
    api.reasoning_effort = selection.effort;
  }
}

namespace {
void PublishActiveRoute(const Api& api) {
  SettingValues route;
  for (auto& [name, value] :
       RouteEnvironment(ResolveSideRoute(api, {}, {}, "").route)) {
    route[name] = std::move(value);
  }
  PublishRoute(std::move(route));
}

void ResetRouteCapabilities(Api& api) {
  const json features = api.capabilities.model_features;
  const json modalities = api.capabilities.input_modalities;
  api.capabilities = CapabilitiesForRoute(api.wire_api, api.features);
  api.capabilities.SetModelFeatures(features);
  api.capabilities.SetInputModalities(modalities, api.wire_api);
}

}  // namespace

EnvironmentOverrides RouteEnvironment(const Route& route) {
  std::string features;
  for (const json& name : FeatureNames(route.features)) {
    features += (features.empty() ? "" : ",") + name.get<std::string>();
  }
  return {
      {"UAGENT_BASE_URL", route.base_url},
      {"UAGENT_MODEL", route.model},
      {"UAGENT_INTERNAL_MODEL_FEATURES", JsonDump(route.model_features)},
      {"UAGENT_REASONING_EFFORT", route.reasoning_effort},
      {"UAGENT_CONTEXT", std::to_string(route.ctx_window)},
      {"UAGENT_INTERNAL_WIRE_API", WireApiName(route.wire_api)},
      {"UAGENT_INTERNAL_FEATURES", features},
      {"UAGENT_INTERNAL_ENDPOINT_OPTIONS", JsonDump(route.options)},
      {"UAGENT_INTERNAL_HOSTED_TOOLS", JsonDump(route.hosted_tools)},
  };
}

RouteResolution ResolveSideRoute(const Api& api,
                                 const std::vector<Route>& routes,
                                 const std::vector<Endpoint>& providers,
                                 const std::string& requested) {
  ModelSelection parsed = ParseModelSelection(requested);
  RouteResolution resolved;
  resolved.selection = parsed.base.empty() ? api.model : requested;
  Route& route = resolved.route;
  route = api;
  route.model_features = api.capabilities.model_features;
  if (!parsed.base.empty()) {
    route.model = parsed.base;
    if (std::optional<Route> named =
            ResolveModelRoute(routes, providers, parsed.base)) {
      route = std::move(*named);
    } else {
      // A bare model id on the parent's provider also lands here; only the
      // execution paths decide whether that is usable.
      resolved.unresolved = true;
      if (route.model != api.model) route.model_features = nullptr;
    }
  }
  route.supported_reasoning_efforts.clear();
  if (route.api_key.empty()) route.api_key = kPlaceholderApiKey;
  // A suffix is the most specific statement of intent, so it wins over both the
  // route's configured value and the session default.
  if (!parsed.effort.empty()) route.reasoning_effort = parsed.effort;
  if (route.model.empty()) {
    resolved.none = "no model set; choose one with /models";
  }
  return resolved;
}

namespace {
bool AcceptsImages(const ModelInfo& info) {
  return info.input_modalities.is_array() &&
         std::ranges::any_of(info.input_modalities, [](const json& modality) {
           return modality.is_string() &&
                  modality.get<std::string>() == "image";
         });
}

// The first model of the conversation's own endpoint that declares image
// input. The cached catalogue answers; a live query fills it when absent.
std::optional<std::string> VisionModelOnEndpoint(const Api& api) {
  std::optional<std::vector<ModelInfo>> models = CachedModels(api);
  if (!models) {
    Api catalog_api(api.config);
    static_cast<Endpoint&>(catalog_api) = api;
    catalog_api.capabilities = api.capabilities;
    models = QueryModels(catalog_api, /*abortable=*/false, /*timeout_s=*/8);
  }
  if (models) {
    for (const ModelInfo& info : *models) {
      if (AcceptsImages(info)) {
        return info.id;
      }
    }
  }
  return std::nullopt;
}
}  // namespace

RouteResolution ResolveImageRoute(const Api& api,
                                  const std::vector<Route>& routes,
                                  const std::vector<Endpoint>& providers,
                                  const std::string& setting) {
  RouteResolution resolved = ResolveSideRoute(api, routes, providers, setting);
  if (!resolved.none.empty() || !setting.empty() ||
      api.capabilities.image_input) {
    return resolved;
  }
  std::optional<std::string> vision = VisionModelOnEndpoint(api);
  if (!vision) {
    resolved.none =
        "no model on this endpoint accepts images; set image.model "
        "(UAGENT_IMAGE_MODEL)";
    return resolved;
  }
  resolved.selection = resolved.route.model = *vision;
  resolved.route.reasoning_effort.clear();
  resolved.route.ctx_window = 0;
  resolved.route.model_features = nullptr;
  return resolved;
}

RouteResolution ResolveReviewRoute(const Api& api,
                                   const std::vector<Endpoint>& providers,
                                   const std::string& setting) {
  auto serves = [](const Endpoint& endpoint) {
    return endpoint.features.Has(Feature::kDecisions) &&
           !endpoint.Option("decisions_url").empty() &&
           !endpoint.api_key.empty() && endpoint.api_key != kPlaceholderApiKey;
  };
  const Endpoint* chosen = nullptr;
  std::string model = setting;
  const size_t slash = setting.find('/');
  if (const Endpoint* named =
          slash == std::string::npos
              ? nullptr
              : FindEndpoint(providers, setting.substr(0, slash));
      named && serves(*named)) {
    chosen = named;
    model = setting.substr(slash + 1);
  } else if (serves(api)) {
    chosen = &api;
  } else {
    for (const Endpoint& provider : providers) {
      if (serves(provider)) {
        chosen = &provider;
        break;
      }
    }
  }
  RouteResolution resolved;
  if (!chosen) {
    resolved.none =
        "no configured endpoint serves decision models (OpenRouter does, "
        "with OPENROUTER_API_KEY set)";
    return resolved;
  }
  static_cast<Endpoint&>(resolved.route) = *chosen;
  if (model.empty()) model = chosen->Option("decisions_model");
  if (model.empty()) {
    resolved.none =
        "the endpoint names no decision model; set approval.reviewerModel";
  }
  resolved.route.model = resolved.selection = model;
  return resolved;
}

std::string ComposeSelection(const std::string& scope, const std::string& model,
                             const std::string& effort) {
  std::string selection = scope.empty() ? "" : scope + "/";
  selection += model;
  if (!effort.empty()) selection += ":" + effort;
  return selection;
}

std::string RouteSelection(const Route& route,
                           const std::vector<Endpoint>& providers) {
  if (route.model.empty()) return {};
  return ComposeSelection(ProviderScope(route.base_url, providers), route.model,
                          route.reasoning_effort);
}

std::string ChosenSelection(const Api& api,
                            const std::vector<Endpoint>& providers) {
  if (api.route_name.empty() || api.route_model != api.model) {
    return RouteSelection(api, providers);
  }
  return ComposeSelection("", api.route_name, api.reasoning_effort);
}

void ApplyRoute(Api& api, const Route& route) {
  static_cast<Route&>(api) = route;
  if (api.api_key.empty()) api.api_key = kPlaceholderApiKey;
  api.route_model = api.model;
  api.capabilities = CapabilitiesForRoute(api.wire_api, api.features);
  api.capabilities.SetModelFeatures(api.model_features);
}

bool SupportsReasoningEffort(const Api& api, std::string_view effort) {
  return effort.empty() || api.supported_reasoning_efforts.empty() ||
         std::find(api.supported_reasoning_efforts.begin(),
                   api.supported_reasoning_efforts.end(),
                   effort) != api.supported_reasoning_efforts.end();
}

void ActivateRoute(Api& api) {
  ResetRouteCapabilities(api);
  PublishActiveRoute(api);
}

ProviderSetup ConfigureProvider(Api& api) {
  api.base_url = StripTrailingSlashes(SettingText(Cfg("UAGENT_BASE_URL")));
  api.api_key = SettingText(Cfg("UAGENT_API_KEY"));
  if (api.api_key.empty()) api.api_key = kPlaceholderApiKey;
  ModelSelection requested =
      ParseModelSelection(SettingText(Cfg("UAGENT_MODEL")));
  api.model = requested.base;
  const std::string configured_effort =
      SettingText(Cfg("UAGENT_REASONING_EFFORT"));
  api.reasoning_effort = configured_effort;
  api.ctx_window = LongSetting(Cfg("UAGENT_CONTEXT"));
  std::string wire_setting =
      EnvStr("UAGENT_INTERNAL_WIRE_API", "chat_completions");
  std::optional<WireApi> configured_wire = ParseWireApi(wire_setting);
  api.wire_api = configured_wire.value_or(WireApi::kChatCompletions);
  // A parent hands its child the features and options of the route it chose;
  // anyone else gets what the address declares.
  std::string metadata_error;
  if (!configured_wire) metadata_error = "invalid wire API: " + wire_setting;
  const bool inherited = std::getenv("UAGENT_INTERNAL_FEATURES") != nullptr;
  if (inherited) {
    json names = json::array();
    for (std::string name :
         SplitPathList(EnvStr("UAGENT_INTERNAL_FEATURES"), ',')) {
      name = Trim(name);
      if (!name.empty()) names.push_back(std::move(name));
    }
    std::string unknown;
    if (!ParseFeatures(names, api.features, unknown)) {
      if (!metadata_error.empty()) metadata_error += "; ";
      metadata_error += "invalid endpoint feature: " + unknown;
    }
    api.options =
        json::parse(EnvStr("UAGENT_INTERNAL_ENDPOINT_OPTIONS"), nullptr, false);
    if (api.options.is_discarded()) api.options = nullptr;
    api.hosted_tools = HostedToolDefinitions(
        json::parse(EnvStr("UAGENT_INTERNAL_HOSTED_TOOLS"), nullptr, false));
  }
  const json env_features =
      json::parse(EnvStr("UAGENT_INTERNAL_MODEL_FEATURES"), nullptr, false);
  const auto start_capabilities = [&] {
    api.capabilities = CapabilitiesForRoute(api.wire_api, api.features);
    api.capabilities.SetModelFeatures(env_features);
  };
  start_capabilities();

  ProviderCatalog catalog = SessionProviderCatalog();
  ProviderSetup setup{
      std::move(catalog.models), std::move(catalog.providers), {}};
  bool route_metadata_validated = false;
  if (std::optional<Route> route =
          ResolveModelRoute(setup.routes, setup.providers, api.model)) {
    ApplyRoute(api, *route);
    route_metadata_validated = true;
  } else if (!inherited && !api.base_url.empty()) {
    DeclareEndpoint(api, setup.providers);
    start_capabilities();
  }
  // Command-line/config suffixes are the most specific route policy. Apply
  // them after a named route so they override its defaults without becoming
  // part of the model ID sent over the active wire API.
  ApplySelectionPolicy(api, requested);
  if (requested.effort.empty() && !configured_effort.empty() &&
      SupportsReasoningEffort(api, configured_effort)) {
    api.reasoning_effort = configured_effort;
  }
  if (api.base_url.empty() && metadata_error.empty()) {
    // No address: the first well-known endpoint whose key is set.
    if (const Endpoint* endpoint = DefaultEndpoint(setup.providers)) {
      static_cast<Endpoint&>(api) = *endpoint;
      if (api.ctx_window == 0) api.ctx_window = endpoint->context;
      start_capabilities();
      api.capabilities.SetModelFeatures(endpoint->model_features);
      route_metadata_validated = true;
    }
  }
  if (!metadata_error.empty() && !route_metadata_validated) {
    setup.warning = std::move(metadata_error);
    api.base_url.clear();
  }
  if (!ValidEffort(api.reasoning_effort) ||
      !SupportsReasoningEffort(api, api.reasoning_effort)) {
    if (!setup.warning.empty()) setup.warning += "; ";
    setup.warning +=
        "ignoring invalid reasoning effort: " + api.reasoning_effort;
    api.reasoning_effort.clear();
    OverrideSetting("UAGENT_REASONING_EFFORT", "");
  }
  return setup;
}

}  // namespace uagent
