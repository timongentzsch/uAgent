// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
namespace {

// Catalog model features with configured route or provider features on top:
// what an operator declared wins over what a catalog reports.
json MergeFeatures(const json& catalog, const json& configured) {
  if (!configured.is_object()) return catalog;
  json merged = catalog.is_object() ? catalog : json::object();
  merged.update(configured);
  return merged;
}

// A bare id the active endpoint's own catalogue lists: the picker offers these
// for an endpoint no configured provider names.
bool EndpointOffers(const Api& api, const std::string& id) {
  const auto models = CachedModels(api);
  return models && std::ranges::any_of(*models, [&](const ModelInfo& info) {
           return info.id == id;
         });
}

}  // namespace

// Two selections naming the same endpoint, model and wire are the same route
// however they were spelled; only one of them is offered.
std::string RouteIdentity(const Route& route) {
  return route.base_url + "\n" + route.model + "\n" +
         WireApiName(route.wire_api) + "\n" +
         std::to_string(route.features.bits) + "\n" + route.options.dump();
}

ModelSelection ParseModelSelection(const std::string& selection) {
  ModelSelection parsed;
  parsed.base = Trim(selection);
  for (;;) {
    size_t colon = parsed.base.rfind(':');
    if (colon == std::string::npos || colon + 1 == parsed.base.size()) break;
    std::string suffix = parsed.base.substr(colon + 1);
    if (!parsed.effort.empty() || !ValidEffort(suffix)) break;
    parsed.effort = std::move(suffix);
    parsed.base.resize(colon);
  }
  return parsed;
}

bool ValidEffort(const std::string& effort) {
  return Cfg("UAGENT_REASONING_EFFORT").Accepts(effort);
}

std::optional<Route> ResolveModelRoute(const std::vector<Route>& routes,
                                       const std::vector<Endpoint>& providers,
                                       const std::string& selection) {
  for (const Route& route : routes) {
    if (route.route_name == selection) return route;
  }
  size_t slash = selection.find('/');
  if (slash == std::string::npos || slash + 1 == selection.size()) {
    return std::nullopt;
  }
  const Endpoint* provider =
      FindEndpoint(providers, selection.substr(0, slash));
  if (!provider) return std::nullopt;
  Route route;
  static_cast<Endpoint&>(route) = *provider;
  route.route_name = selection;
  route.model = selection.substr(slash + 1);
  route.ctx_window = provider->context;
  return route;
}

bool CanUseRawModel(const Api& api, std::string_view name) {
  return api.features.Has(Feature::kRawVendorIds) &&
         name.find('/') != std::string_view::npos;
}

bool ProbeModel(Api& api, bool discover_efforts) {
  if (api.model.empty()) {
    return false;
  }
  if (api.ctx_window > 0 &&
      (!discover_efforts || !api.supported_reasoning_efforts.empty()) &&
      !(api.wire_api == WireApi::kAnthropicMessages &&
        api.capabilities.model_features.is_null())) {
    return true;
  }
  // Discover effort support on demand, without delaying a configured startup
  // for optional metadata. Unavailable metadata leaves the route usable.
  auto started = std::chrono::steady_clock::now();
  // The catalogue lists an id without the routing suffixes (`:nitro`) an
  // endpoint takes on any id: the exact id wins, else the id without them.
  const auto listed = [&](const std::vector<ModelInfo>& models) {
    std::string id = api.model;
    for (;;) {
      auto found = std::ranges::find(models, id, &ModelInfo::id);
      if (found != models.end()) return &*found;
      const size_t colon = id.rfind(':');
      if (!api.features.Has(Feature::kModelVariants) ||
          colon == std::string::npos) {
        return static_cast<const ModelInfo*>(nullptr);
      }
      id.resize(colon);
    }
  };
  // A cached catalogue that lacks the chosen model may predate it.
  auto models = CachedModels(api);
  if (!models || !listed(*models)) {
    models = QueryModels(api, /*abortable=*/false, 8);
  }
  if (models && !models->empty()) {
    if (const ModelInfo* info = listed(*models)) {
      if (api.ctx_window == 0) api.ctx_window = info->context;
      api.supported_reasoning_efforts = info->efforts;
      api.capabilities.SetInputModalities(info->input_modalities, api.wire_api);
      api.capabilities.SetModelFeatures(info->features);
      if (!SupportsReasoningEffort(api, api.reasoning_effort)) {
        api.reasoning_effort.clear();
      }
    }
  }
  DebugLog("models_probe", {{"duration_ms", ElapsedMs(started)},
                            {"models_offered", models ? models->size() : 0},
                            {"model", api.model},
                            {"context_window", api.ctx_window}});
  return true;
}

std::string SelectModel(Api& api, const std::vector<Route>& routes,
                        const std::vector<Endpoint>& providers,
                        const std::string& name) {
  const std::string previous_url = api.base_url;
  const std::string previous_model = api.model;
  const int64_t previous_context = api.ctx_window;
  const auto previous_efforts = api.supported_reasoning_efforts;
  const auto previous_capabilities = api.capabilities;
  const WireApi previous_wire = api.wire_api;
  const FeatureSet previous_features = api.features;
  const json previous_options = api.options;
  ModelSelection selection = ParseModelSelection(name);
  std::string selected = selection.base;
  if (std::optional<Route> route =
          ResolveModelRoute(routes, providers, selection.base)) {
    ApplyRoute(api, *route);
    selected = route->route_name;
  } else if (CanUseRawModel(api, selection.base) ||
             EndpointOffers(api, selection.base)) {
    api.model = selection.base;
    api.route_name.clear();
    api.ctx_window = 0;
    api.reasoning_effort.clear();
    api.supported_reasoning_efforts.clear();
    api.capabilities = CapabilitiesForRoute(api.wire_api, api.features);
  } else {
    return "";
  }
  if (api.base_url == previous_url && api.model == previous_model) {
    if (api.ctx_window == 0) api.ctx_window = previous_context;
    api.supported_reasoning_efforts = previous_efforts;
    if (api.features == previous_features && api.wire_api == previous_wire &&
        api.options == previous_options) {
      api.capabilities = previous_capabilities;
    }
  } else {
    ProbeModel(api, /*discover_efforts=*/true);
  }
  ApplySelectionPolicy(api, selection);
  std::string effort = api.reasoning_effort == selection.effort
                           ? selection.effort
                           : std::string();
  return ComposeSelection("", selected, effort);
}

std::string NormalizeModelQuery(std::string query) {
  query = Trim(query);
  if (query == "all" || query == "*") return "";
  if (query.ends_with("/*")) query.pop_back();
  return query;
}

// Users type display names with spaces ("Muse Spark") while ids use hyphens
// ("muse-spark"). Unify separators before the fallback comparison so both
// spellings match; the exact substring check above runs first and is
// unchanged for id-style queries.
std::string SearchKey(std::string text) {
  for (char& c : text) {
    if (c == ' ' || c == '_' || c == '-') c = '-';
  }
  return text;
}

bool MatchesModelQuery(const std::string& haystack, const std::string& query) {
  if (ContainsCaseInsensitive(haystack, query)) return true;
  return ContainsCaseInsensitive(SearchKey(haystack), SearchKey(query));
}

ModelSearch SearchModels(const Api& api, const std::vector<Route>& routes,
                         const std::vector<Endpoint>& providers,
                         std::string query) {
  query = NormalizeModelQuery(std::move(query));
  ModelSearch result;
  std::set<std::string> selections;
  std::set<std::string> route_identities;
  for (const Route& route : routes) {
    if (!MatchesModelQuery(route.route_name + " " + route.model, query)) {
      continue;
    }
    if (!selections.insert(route.route_name).second) continue;
    route_identities.insert(RouteIdentity(route));
    ModelInfo info{route.model, route.reasoning_effort,
                   route.supported_reasoning_efforts, route.ctx_window};
    result.matches.push_back({route, std::move(info)});
  }

  // Configured providers plus, when it is not one of them, the active
  // endpoint — which has no name, and therefore no `provider/model` prefix.
  std::vector<Endpoint> catalogs = providers;
  bool active_is_named = std::any_of(
      catalogs.begin(), catalogs.end(),
      [&](const Endpoint& source) { return source.base_url == api.base_url; });
  if (!active_is_named && !api.base_url.empty()) {
    Endpoint active = api;
    active.name.clear();
    active.context = api.ctx_window;
    catalogs.push_back(std::move(active));
  }

  using CatalogModels = std::optional<std::vector<ModelInfo>>;
  result.queried = catalogs.size();
  std::vector<CatalogModels> responses(catalogs.size());
  std::atomic<size_t> next{0};
  size_t worker_count =
      std::min(catalogs.size(), static_cast<size_t>(ToolConcurrency()));
  std::vector<std::future<void>> workers;
  workers.reserve(worker_count);
  for (size_t worker = 0; worker < worker_count; ++worker) {
    workers.push_back(std::async(std::launch::async, [&] {
      for (size_t index; !AbortRequested() &&
                         (index = next.fetch_add(1)) < catalogs.size();) {
        Api catalog_api(api.config);
        static_cast<Endpoint&>(catalog_api) = catalogs[index];
        catalog_api.capabilities =
            CapabilitiesForRoute(catalog_api.wire_api, catalog_api.features);
        responses[index] = QueryModels(catalog_api, /*abortable=*/true);
      }
    }));
  }
  for (auto& worker : workers) worker.get();
  for (size_t i = 0; i < catalogs.size(); ++i) {
    const Endpoint& source = catalogs[i];
    CatalogModels models = std::move(responses[i]);
    if (!models) {
      result.unavailable.push_back(
          source.name.empty() ? UrlHost(source.base_url) : source.name);
      continue;
    }
    for (ModelInfo& info : *models) {
      std::string selection =
          source.name.empty() ? info.id : source.name + "/" + info.id;
      Route route;
      static_cast<Endpoint&>(route) = source;
      route.route_name = selection;
      route.model = info.id;
      const std::string identity = RouteIdentity(route);
      // Aliases keep their configured routing/defaults, but must not hide the
      // live model's capabilities behind the generic effort fallback.
      for (ModelCandidate& candidate : result.matches) {
        Route& alias = candidate.route;
        if (RouteIdentity(alias) != identity) continue;
        alias.supported_reasoning_efforts = info.efforts;
        alias.model_features =
            MergeFeatures(info.features, alias.model_features);
        if (alias.ctx_window == 0) alias.ctx_window = info.context;
        candidate.info = info;
        candidate.info.context = alias.ctx_window;
      }
      if (!MatchesModelQuery(selection + " " + info.name, query) ||
          !selections.insert(selection).second ||
          !route_identities.insert(identity).second) {
        continue;
      }
      route.ctx_window = info.context > 0 ? info.context : source.context;
      route.supported_reasoning_efforts = info.efforts;
      route.reasoning_effort = info.default_effort;
      route.model_features = MergeFeatures(info.features, route.model_features);
      if (info.context == 0) info.context = route.ctx_window;
      result.matches.push_back({std::move(route), std::move(info)});
    }
  }
  std::sort(result.matches.begin(), result.matches.end(),
            [](const ModelCandidate& a, const ModelCandidate& b) {
              return a.route.route_name < b.route.route_name;
            });
  return result;
}

json ModelCatalogue(Api& api, const std::vector<Route>& routes,
                    const std::vector<Endpoint>& providers,
                    const std::string& query) {
  ModelSearch search = SearchModels(api, routes, providers, query);
  json models = json::array();
  for (const ModelCandidate& candidate : search.matches) {
    bool active = candidate.route.base_url == api.base_url &&
                  candidate.route.model == api.model;
    if (active) {
      api.supported_reasoning_efforts = candidate.info.efforts;
      api.capabilities.SetInputModalities(candidate.info.input_modalities,
                                          api.wire_api);
      api.capabilities.SetModelFeatures(candidate.route.model_features);
    }
    models.push_back(
        {{"value", candidate.route.route_name},
         {"label",
          ComposeSelection(ProviderScope(candidate.route.base_url, providers),
                           candidate.route.model, "")},
         {"name", candidate.info.name},
         {"efforts", candidate.info.efforts},
         {"input_modalities", candidate.info.input_modalities},
         {"default_effort", candidate.info.default_effort},
         {"active", active}});
  }
  return {{"models", models}, {"unavailable", search.unavailable}};
}

}  // namespace uagent
