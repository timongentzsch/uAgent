// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/providers.h"

namespace uagent {
void ApplySelectionPolicy(Api& api, const ModelSelection& selection) {
  if (!selection.variant.empty() && api.capabilities.model_variants) {
    api.config.openrouter_variant = selection.variant;
  }
  if (!selection.effort.empty() &&
      SupportsReasoningEffort(api, selection.effort)) {
    api.reasoning_effort = selection.effort;
  }
}

namespace {
void ExportRoute(const Api& api) {
  setenv("UAGENT_BASE_URL", api.base_url.c_str(), 1);
  setenv("UAGENT_MODEL", api.model.c_str(), 1);
  setenv("UAGENT_MODEL_FEATURES",
         JsonDump(api.capabilities.model_features).c_str(), 1);
  setenv("UAGENT_REASONING_EFFORT", api.reasoning_effort.c_str(), 1);
  setenv("UAGENT_OPENROUTER_VARIANT", api.config.openrouter_variant.c_str(), 1);
  setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
  setenv("UAGENT_PROVIDER_PROTOCOL",
         ProviderProtocolName(api.capabilities.protocol), 1);
  setenv("UAGENT_WIRE_API", WireApiName(api.capabilities.wire_api), 1);
  setenv("UAGENT_HOSTED_TOOLS",
         api.capabilities.hosted_web_search ? "web_search" : "", 1);
}

void ResetRouteCapabilities(Api& api) {
  const json features = api.capabilities.model_features;
  api.capabilities = CapabilitiesForRoute(
      api.capabilities.protocol, api.base_url, api.capabilities.wire_api,
      api.capabilities.hosted_web_search);
  api.capabilities.SetModelFeatures(features);
}

}  // namespace
SideRoute ResolveSideRoute(const Api& api,
                           const std::vector<ModelRoute>& routes,
                           const std::vector<NamedProvider>& providers,
                           const std::string& requested) {
  ModelSelection parsed = ParseModelSelection(requested);
  SideRoute resolved;
  resolved.selection = parsed.base.empty() ? api.model : requested;
  resolved.model = parsed.base.empty() ? api.model : parsed.base;
  resolved.base_url = api.base_url;
  resolved.api_key = api.api_key;
  resolved.effort = api.reasoning_effort;
  resolved.variant = api.config.openrouter_variant;
  resolved.context = api.ctx_window;
  resolved.protocol = api.capabilities.protocol;
  resolved.wire_api = api.capabilities.wire_api;
  resolved.hosted_web_search = api.capabilities.hosted_web_search;
  resolved.features = api.capabilities.model_features;
  if (!parsed.base.empty()) {
    if (std::optional<ModelRoute> route =
            ResolveModelRoute(routes, providers, parsed.base)) {
      resolved.base_url = route->base_url;
      resolved.api_key =
          route->api_key.empty() ? kPlaceholderApiKey : route->api_key;
      resolved.model = route->model;
      resolved.effort = route->effort;
      resolved.variant.clear();
      resolved.context = route->context;
      resolved.protocol = route->protocol;
      resolved.wire_api = route->wire_api;
      resolved.hosted_web_search = route->hosted_web_search;
      resolved.features = route->features;
    } else {
      // A bare model id on the parent's provider also lands here; only the
      // execution paths decide whether that is usable.
      resolved.unresolved = true;
      if (resolved.model != api.CatalogModel()) resolved.features = nullptr;
    }
  }
  // A suffix is the most specific statement of intent, so it wins over both the
  // route's configured value and the session default.
  if (!parsed.variant.empty()) resolved.variant = parsed.variant;
  if (!parsed.effort.empty()) resolved.effort = parsed.effort;
  return resolved;
}

void ApplyRoute(Api& api, const ModelRoute& route) {
  api.base_url = route.base_url;
  api.api_key = route.api_key.empty() ? kPlaceholderApiKey : route.api_key;
  api.model = route.model;
  api.reasoning_effort = route.effort;
  api.supported_reasoning_efforts = route.supported_efforts;
  api.config.openrouter_variant.clear();
  api.ctx_window = route.context;
  api.capabilities = CapabilitiesForRoute(
      route.protocol, route.base_url, route.wire_api, route.hosted_web_search);
  api.capabilities.SetInputModalities(route.input_modalities);
  api.capabilities.SetModelFeatures(route.features);
}

std::string RouteSelection(const Api& api,
                           const std::vector<NamedProvider>& providers) {
  // CatalogModel strips a routing variant the request appends, so the suffixes
  // are added once and in schema order.
  return ComposeSelection(
      ProviderScope(api.base_url, providers), api.CatalogModel(),
      api.capabilities.model_variants ? api.config.openrouter_variant
                                      : std::string(),
      api.reasoning_effort);
}

std::string RouteSelection(const SideRoute& route,
                           const std::vector<NamedProvider>& providers) {
  return ComposeSelection(ProviderScope(route.base_url, providers), route.model,
                          route.variant, route.effort);
}

void ApplySideRoute(Api& api, const SideRoute& route) {
  api.base_url = route.base_url;
  api.api_key = route.api_key.empty() ? kPlaceholderApiKey : route.api_key;
  api.model = route.model;
  api.reasoning_effort = route.effort;
  api.supported_reasoning_efforts.clear();
  api.ctx_window = route.context;
  api.capabilities = CapabilitiesForRoute(
      route.protocol, route.base_url, route.wire_api, route.hosted_web_search);
  api.capabilities.SetModelFeatures(route.features);
  api.config.openrouter_variant = route.variant;
}

bool SupportsReasoningEffort(const Api& api, std::string_view effort) {
  return effort.empty() || api.supported_reasoning_efforts.empty() ||
         std::find(api.supported_reasoning_efforts.begin(),
                   api.supported_reasoning_efforts.end(),
                   effort) != api.supported_reasoning_efforts.end();
}

void ActivateRoute(Api& api) {
  ResetRouteCapabilities(api);
  ExportRoute(api);
}

ProviderSetup ConfigureProvider(Api& api) {
  api.base_url = StripTrailingSlashes(EnvStr("UAGENT_BASE_URL"));
  api.api_key = EnvStr("UAGENT_API_KEY", kPlaceholderApiKey);
  ModelSelection requested = ParseModelSelection(EnvStr("UAGENT_MODEL"));
  api.model = requested.base;
  const std::string configured_effort = EnvStr("UAGENT_REASONING_EFFORT");
  const std::string configured_variant = api.config.openrouter_variant;
  api.reasoning_effort = configured_effort;
  api.ctx_window = ContextWindow();
  std::string protocol_setting = EnvStr("UAGENT_PROVIDER_PROTOCOL");
  std::string wire_setting = EnvStr("UAGENT_WIRE_API", "chat_completions");
  std::optional<WireApi> configured_wire = ParseWireApi(wire_setting);
  WireApi wire_api = configured_wire.value_or(WireApi::kChatCompletions);
  std::optional<ProviderProtocol> configured_protocol;
  if (!protocol_setting.empty()) {
    configured_protocol = ParseProviderProtocol(protocol_setting);
  }
  ProviderProtocol protocol =
      configured_protocol.value_or(ProviderProtocol::kOpenAi);
  json hosted_tools = json::array();
  for (std::string tool : SplitPathList(EnvStr("UAGENT_HOSTED_TOOLS"), ',')) {
    tool = Trim(tool);
    if (!tool.empty()) hosted_tools.push_back(std::move(tool));
  }
  api.capabilities =
      CapabilitiesForRoute(protocol, api.base_url, wire_api,
                           HasHostedTool(hosted_tools, HostedTool::kWebSearch));
  api.capabilities.SetModelFeatures(
      json::parse(EnvStr("UAGENT_MODEL_FEATURES"), nullptr, false));

  ProviderCatalog catalog = SessionProviderCatalog();
  ProviderSetup setup{
      std::move(catalog.models), std::move(catalog.providers), {}};
  std::string metadata_error;
  if (!configured_wire) metadata_error = "invalid wire API: " + wire_setting;
  if (!protocol_setting.empty() && !configured_protocol) {
    if (!metadata_error.empty()) metadata_error += "; ";
    metadata_error += "invalid provider protocol: " + protocol_setting;
  }
  bool route_metadata_validated = false;
  if (std::optional<ModelRoute> route =
          ResolveModelRoute(setup.routes, setup.providers, api.model)) {
    ApplyRoute(api, *route);
    route_metadata_validated = true;
  } else if (api.model.empty()) {  // no explicit model: restore the last /model
    ModelPreference preference = LoadModelPreference();
    ModelSelection preferred = ParseModelSelection(preference.selection);
    if (preference.route) {
      if (std::optional<ModelRoute> saved = ResolveModelRoute(
              setup.routes, setup.providers, preferred.base)) {
        ApplyRoute(api, *saved);
        route_metadata_validated = true;
        ApplySelectionPolicy(api, preferred);
      }
    } else if (!preference.selection.empty()) {
      bool same_provider = api.base_url == preference.base_url;
      if (api.base_url.empty()) {
        const ProviderTemplate* provider =
            FindProviderTemplateForUrl(preference.base_url);
        same_provider = provider && !EnvStr(provider->api_key_env).empty();
      }
      if (same_provider) {
        api.model = preferred.base;
        ApplySelectionPolicy(api, preferred);
      }
    }
  }
  // Command-line/config suffixes are the most specific route policy. Apply
  // them after a named route so they override its defaults without becoming
  // part of the model ID sent over the active wire API.
  ApplySelectionPolicy(api, requested);
  if (requested.effort.empty() && !configured_effort.empty() &&
      SupportsReasoningEffort(api, configured_effort)) {
    api.reasoning_effort = configured_effort;
  }
  if (requested.variant.empty() && !configured_variant.empty() &&
      api.capabilities.model_variants) {
    api.config.openrouter_variant = configured_variant;
  }
  if (api.base_url.empty() && metadata_error.empty()) {
    route_metadata_validated =
        ApplyProviderTemplate(api, DefaultProviderTemplate());
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
    setenv("UAGENT_REASONING_EFFORT", "", 1);
  }
  return setup;
}

}  // namespace uagent
