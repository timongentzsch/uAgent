// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/limits.h"
#include "include/providers/providers.h"

namespace uagent {
void ApplySelectionPolicy(Api& api, const ModelSelection& selection) {
  if (!selection.effort.empty() &&
      SupportsReasoningEffort(api, selection.effort)) {
    api.reasoning_effort = selection.effort;
  }
}

namespace {
void ResetRouteCapabilities(Api& api) {
  const json features = api.capabilities.model_features;
  const json modalities = api.capabilities.input_modalities;
  api.capabilities = CapabilitiesForRoute(api.wire_api, api.features);
  api.capabilities.SetModelFeatures(features);
  api.capabilities.SetInputModalities(modalities, api.wire_api);
}

}  // namespace

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
      if (route.model != api.model) route.model_features = nullptr;
    }
  }
  route.supported_reasoning_efforts.clear();
  if (route.api_key.empty()) route.api_key = kPlaceholderApiKey;
  // A suffix is the most specific statement of intent, so it wins over both the
  // route's configured value and the session default.
  if (!parsed.effort.empty()) route.reasoning_effort = parsed.effort;
  if (route.model.empty()) {
    resolved.none = "no model set; choose one with /model";
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
    models =
        QueryModels(catalog_api, /*abortable=*/false, kWaitedModelListSeconds);
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
           !endpoint.Option("decisions_url").empty() && HasKey(endpoint);
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
  }
  RouteResolution resolved;
  if (api.FreeOnly() && setting.empty()) {
    resolved.none =
        "free-only provider has no automatic paid approval reviewer";
    return resolved;
  }
  if (!chosen) {
    // Another provider is never sent a command it was not named for.
    const auto other = std::ranges::find_if(providers, serves);
    resolved.none =
        other == providers.end()
            ? "no provider serves decision models; one at openrouter.ai does"
            : "this conversation's provider serves no decision model; set "
              "approval.reviewerModel to " +
                  other->name + "/ to review at " + other->name;
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
  return ComposeSelection(ProviderScope(route, providers), route.model,
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
  // What a setting read finds for the route in use, where no setting names it.
  PublishRoute(
      {{Cfg("UAGENT_MODEL").EnvName(), api.model},
       {Cfg("UAGENT_REASONING_EFFORT").EnvName(), api.reasoning_effort},
       {Cfg("UAGENT_CONTEXT").EnvName(), std::to_string(api.ctx_window)}});
}

ProviderSetup ConfigureProvider(Api& api) {
  api.api_key = kPlaceholderApiKey;
  ModelSelection requested =
      ParseModelSelection(SettingText(Cfg("UAGENT_MODEL")));
  api.model = requested.base;
  const std::string configured_effort =
      SettingText(Cfg("UAGENT_REASONING_EFFORT"));
  api.reasoning_effort = configured_effort;
  const int64_t configured_context = LongSetting(Cfg("UAGENT_CONTEXT"));
  api.ctx_window = configured_context;
  api.capabilities = CapabilitiesForRoute(api.wire_api, api.features);

  ProviderCatalog catalog = LoadProviderCatalog();
  ProviderSetup setup{
      std::move(catalog.models), std::move(catalog.providers), {}};
  if (std::optional<Route> route =
          ResolveModelRoute(setup.routes, setup.providers, api.model)) {
    ApplyRoute(api, *route);
    // A window set for all conversations wins over the provider's profile.
    if (configured_context > 0) api.ctx_window = configured_context;
  }
  // A suffix overrides a named route's defaults without becoming part of
  // the model id sent over the wire.
  ApplySelectionPolicy(api, requested);
  if (requested.effort.empty() && !configured_effort.empty() &&
      SupportsReasoningEffort(api, configured_effort)) {
    api.reasoning_effort = configured_effort;
  }
  if (!ValidEffort(api.reasoning_effort) ||
      !SupportsReasoningEffort(api, api.reasoning_effort)) {
    if (!setup.warning.empty()) setup.warning += "; ";
    setup.warning +=
        "ignoring invalid reasoning effort: " + api.reasoning_effort;
    api.reasoning_effort.clear();
    OverrideSetting(Cfg("UAGENT_REASONING_EFFORT").EnvName(), "");
  }
  return setup;
}

}  // namespace uagent
