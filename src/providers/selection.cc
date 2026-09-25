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
namespace {

// Catalog model features with configured route or provider features on top:
// what an operator declared wins over what a catalog reports.
json MergeFeatures(const json& catalog, const json& configured) {
  if (!configured.is_object()) return catalog;
  json merged = catalog.is_object() ? catalog : json::object();
  merged.update(configured);
  return merged;
}

}  // namespace

// Two selections naming the same endpoint, model and protocol are the same
// route however they were spelled; only one of them is offered.
std::string RouteIdentity(const std::string& base_url, const std::string& model,
                          ProviderProtocol protocol, WireApi wire_api,
                          bool hosted_web_search) {
  return base_url + "\n" + model + "\n" + ProviderProtocolName(protocol) +
         "\n" + WireApiName(wire_api) + "\n" +
         (hosted_web_search ? "web_search" : "");
}

// The configured provider serving a base URL, else the built-in template that
// matches it, else nothing — a custom endpoint has no name to scope with.
std::string ProviderScope(const std::string& base_url,
                          const std::vector<NamedProvider>& providers) {
  for (const NamedProvider& provider : providers) {
    if (provider.base_url == base_url) return provider.name;
  }
  const ProviderTemplate* provider = FindProviderTemplateForUrl(base_url);
  return provider ? provider->name : std::string();
}

std::string ComposeSelection(const std::string& scope, const std::string& model,
                             const std::string& variant,
                             const std::string& effort) {
  std::string selection = scope.empty() ? "" : scope + "/";
  selection += model;
  if (!variant.empty()) selection += ":" + variant;
  if (!effort.empty()) selection += ":" + effort;
  return selection;
}

std::string ModelPreferencePath() {
  return UagentDir(kConfigDir) + "/model-preference.json";
}

bool PersistableSelection(const std::string& selection) {
  return !selection.empty() &&
         selection.find_first_of("\r\n") == std::string::npos &&
         selection.find('\0') == std::string::npos;
}

ModelPreference LoadModelPreference() {
  std::ifstream input(ModelPreferencePath());
  json saved = json::parse(input, nullptr, false);
  if (!saved.is_object() || JsonValue(saved, "format", 0) != 1) return {};
  ModelPreference preference{
      JsonValue(saved, "selection", ""),
      StripTrailingSlashes(JsonValue(saved, "base_url", "")),
      JsonValue(saved, "route", false)};
  return PersistableSelection(preference.selection) ? preference
                                                    : ModelPreference{};
}

bool SaveModelPreference(const ModelPreference& preference,
                         std::string& error) {
  if (!PersistableSelection(preference.selection)) {
    error = "model selection is not persistable";
    return false;
  }
  json saved = {{"format", 1},
                {"selection", preference.selection},
                {"base_url", preference.base_url},
                {"route", preference.route}};
  return AtomicWriteFile(ModelPreferencePath(), JsonDump(saved, 2) + "\n",
                         kPrivateFileMode,
                         /*preserve_mode=*/false, error);
}

ModelSelection ParseModelSelection(const std::string& selection) {
  ModelSelection parsed;
  parsed.base = Trim(selection);
  for (;;) {
    size_t colon = parsed.base.rfind(':');
    if (colon == std::string::npos || colon + 1 == parsed.base.size()) break;
    std::string suffix = parsed.base.substr(colon + 1);
    if (parsed.effort.empty() && ValidEffort(suffix)) {
      parsed.effort = std::move(suffix);
    } else if (parsed.variant.empty() && ValidOpenRouterVariant(suffix)) {
      parsed.variant = std::move(suffix);
    } else {
      break;
    }
    parsed.base.resize(colon);
  }
  return parsed;
}

bool ValidEffort(const std::string& effort) {
  return effort.empty() ||
         std::find(std::begin(kReasoningEfforts), std::end(kReasoningEfforts),
                   effort) != std::end(kReasoningEfforts);
}

std::optional<ModelRoute> ResolveModelRoute(
    const std::vector<ModelRoute>& routes,
    const std::vector<NamedProvider>& providers, const std::string& selection) {
  for (const ModelRoute& route : routes) {
    if (route.name == selection) return route;
  }
  size_t slash = selection.find('/');
  if (slash == std::string::npos || slash + 1 == selection.size()) {
    return std::nullopt;
  }
  const NamedProvider* provider =
      FindNamedProvider(providers, selection.substr(0, slash));
  if (!provider) return std::nullopt;
  ModelRoute route{selection,
                   provider->base_url,
                   provider->api_key,
                   selection.substr(slash + 1),
                   "",
                   provider->context,
                   provider->protocol,
                   provider->wire_api,
                   provider->hosted_web_search,
                   {}};
  route.features = provider->features;
  return route;
}

bool SaveSelectionSuffix(const std::string& variant, const std::string& effort,
                         std::string& error) {
  ModelPreference preference = LoadModelPreference();
  if (!PersistableSelection(preference.selection)) {
    error = "no saved model preference to update";
    return false;
  }
  ModelSelection parsed = ParseModelSelection(preference.selection);
  preference.selection = ComposeSelection("", parsed.base, variant, effort);
  return SaveModelPreference(preference, error);
}

bool CanUseRawModel(const Api& api, std::string_view name) {
  return api.capabilities.raw_slash_models &&
         name.find('/') != std::string_view::npos;
}

bool ProbeModel(Api& api, bool discover_efforts) {
  if (!api.model.empty() && api.ctx_window > 0 &&
      (!discover_efforts || !api.supported_reasoning_efforts.empty()) &&
      !(api.capabilities.Anthropic() &&
        api.capabilities.model_features.is_null())) {
    return true;
  }
  // Discover effort support on demand, without delaying a configured startup
  // for optional metadata. Unavailable metadata leaves the route usable.
  bool metadata_only = !api.model.empty();
  auto started = std::chrono::steady_clock::now();
  auto models = ParseModels(
      api.Get("/models", /*abortable=*/false, metadata_only ? 8 : 15));
  if (models && !models->empty()) {
    if (api.model.empty()) api.model = models->front().id;
    std::string base = api.CatalogModel();
    for (const ModelInfo& info : *models) {
      if (info.id != api.model && info.id != base) continue;
      if (api.ctx_window == 0) api.ctx_window = info.context;
      api.supported_reasoning_efforts = info.efforts;
      api.capabilities.SetInputModalities(info.input_modalities);
      api.capabilities.SetModelFeatures(info.features);
      if (!SupportsReasoningEffort(api, api.reasoning_effort)) {
        api.reasoning_effort.clear();
      }
      break;
    }
  }
  DebugLog("models_probe", {{"duration_ms", ElapsedMs(started)},
                            {"models_offered", models ? models->size() : 0},
                            {"model", api.model},
                            {"context_window", api.ctx_window}});
  return !api.model.empty();
}

std::string SelectModel(Api& api, const std::vector<ModelRoute>& routes,
                        const std::vector<NamedProvider>& providers,
                        const std::string& name) {
  const std::string previous_url = api.base_url;
  const std::string previous_model = api.CatalogModel();
  const int64_t previous_context = api.ctx_window;
  const auto previous_efforts = api.supported_reasoning_efforts;
  const auto previous_capabilities = api.capabilities;
  ModelSelection selection = ParseModelSelection(name);
  std::string selected = selection.base;
  if (std::optional<ModelRoute> route =
          ResolveModelRoute(routes, providers, selection.base)) {
    ApplyRoute(api, *route);
    selected = route->name;
  } else if (CanUseRawModel(api, selection.base)) {
    api.model = selection.base;
    api.ctx_window = 0;
    api.reasoning_effort.clear();
    api.supported_reasoning_efforts.clear();
    api.config.openrouter_variant.clear();
    api.capabilities = CapabilitiesForRoute(
        api.capabilities.protocol, api.base_url, api.capabilities.wire_api,
        api.capabilities.hosted_web_search);
  } else {
    return "";
  }
  if (api.base_url == previous_url && api.CatalogModel() == previous_model) {
    if (api.ctx_window == 0) api.ctx_window = previous_context;
    api.supported_reasoning_efforts = previous_efforts;
    if (api.capabilities.protocol == previous_capabilities.protocol &&
        api.capabilities.wire_api == previous_capabilities.wire_api) {
      api.capabilities = previous_capabilities;
    }
  } else {
    ProbeModel(api, /*discover_efforts=*/true);
  }
  ApplySelectionPolicy(api, selection);
  std::string variant = api.config.openrouter_variant == selection.variant
                            ? selection.variant
                            : std::string();
  std::string effort = api.reasoning_effort == selection.effort
                           ? selection.effort
                           : std::string();
  return ComposeSelection("", selected, variant, effort);
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

ModelSearch SearchModels(const Api& api, const std::vector<ModelRoute>& routes,
                         const std::vector<NamedProvider>& providers,
                         std::string query) {
  query = NormalizeModelQuery(std::move(query));
  ModelSearch result;
  std::set<std::string> selections;
  std::set<std::string> route_identities;
  for (const ModelRoute& route : routes) {
    if (!MatchesModelQuery(route.name + " " + route.model, query)) {
      continue;
    }
    std::string identity =
        RouteIdentity(route.base_url, route.model, route.protocol,
                      route.wire_api, route.hosted_web_search);
    if (!selections.insert(route.name).second) continue;
    route_identities.insert(std::move(identity));
    ModelInfo info{route.model, route.effort, route.supported_efforts,
                   route.context};
    result.matches.push_back({route.name, route, std::move(info)});
  }

  // Configured providers plus, when it is not one of them, the active
  // endpoint — which has no name, and therefore no `provider/model` prefix.
  std::vector<NamedProvider> catalogs = providers;
  bool active_is_named = std::any_of(catalogs.begin(), catalogs.end(),
                                     [&](const NamedProvider& source) {
                                       return source.base_url == api.base_url;
                                     });
  if (!active_is_named && !api.base_url.empty()) {
    catalogs.push_back({"", api.base_url, api.api_key, api.ctx_window,
                        api.capabilities.protocol, api.capabilities.wire_api,
                        api.capabilities.hosted_web_search});
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
        const NamedProvider& source = catalogs[index];
        Api catalog_api(api.config);
        catalog_api.base_url = source.base_url;
        catalog_api.api_key = source.api_key;
        catalog_api.capabilities =
            CapabilitiesForRoute(source.protocol, source.base_url,
                                 source.wire_api, source.hosted_web_search);
        responses[index] = QueryModels(catalog_api, /*abortable=*/true);
      }
    }));
  }
  for (auto& worker : workers) worker.get();
  for (size_t i = 0; i < catalogs.size(); ++i) {
    const NamedProvider& source = catalogs[i];
    CatalogModels models = std::move(responses[i]);
    if (!models) {
      result.unavailable.push_back(
          source.name.empty() ? UrlHost(source.base_url) : source.name);
      continue;
    }
    for (ModelInfo& info : *models) {
      std::string selection =
          source.name.empty() ? info.id : source.name + "/" + info.id;
      std::string identity =
          RouteIdentity(source.base_url, info.id, source.protocol,
                        source.wire_api, source.hosted_web_search);
      // Aliases keep their configured routing/defaults, but must not hide the
      // live model's capabilities behind the generic effort fallback.
      for (ModelCandidate& candidate : result.matches) {
        ModelRoute& route = candidate.route;
        if (RouteIdentity(route.base_url, route.model, route.protocol,
                          route.wire_api,
                          route.hosted_web_search) != identity) {
          continue;
        }
        route.supported_efforts = info.efforts;
        route.input_modalities = info.input_modalities;
        route.features = MergeFeatures(info.features, route.features);
        if (route.context == 0) route.context = info.context;
        candidate.info = info;
        candidate.info.context = route.context;
      }
      if (!MatchesModelQuery(selection + " " + info.name, query) ||
          !selections.insert(selection).second ||
          !route_identities.insert(std::move(identity)).second) {
        continue;
      }
      int64_t context = info.context > 0 ? info.context : source.context;
      ModelRoute route{selection,
                       source.base_url,
                       source.api_key,
                       info.id,
                       "",
                       context,
                       source.protocol,
                       source.wire_api,
                       source.hosted_web_search,
                       info.efforts};
      route.effort = info.default_effort;
      route.input_modalities = info.input_modalities;
      route.features = MergeFeatures(info.features, source.features);
      if (info.context == 0) info.context = context;
      result.matches.push_back(
          {std::move(selection), std::move(route), std::move(info)});
    }
  }
  std::sort(result.matches.begin(), result.matches.end(),
            [](const ModelCandidate& a, const ModelCandidate& b) {
              return a.selection < b.selection;
            });
  return result;
}

json ModelCatalogue(Api& api, const std::vector<ModelRoute>& routes,
                    const std::vector<NamedProvider>& providers,
                    const std::string& query) {
  ModelSearch search = SearchModels(api, routes, providers, query);
  json models = json::array();
  for (const ModelCandidate& candidate : search.matches) {
    bool active = candidate.route.base_url == api.base_url &&
                  candidate.route.model == api.model;
    if (active) {
      api.supported_reasoning_efforts = candidate.info.efforts;
      api.capabilities.SetInputModalities(candidate.info.input_modalities);
      api.capabilities.SetModelFeatures(candidate.route.features);
    }
    models.push_back(
        {{"value", candidate.selection},
         {"label",
          RouteSelection(SideRoute{.model = candidate.route.model,
                                   .base_url = candidate.route.base_url},
                         providers)},
         {"name", candidate.info.name},
         {"efforts", candidate.info.efforts},
         {"input_modalities", candidate.info.input_modalities},
         {"variants", candidate.route.protocol == ProviderProtocol::kOpenRouter
                          ? json(kOpenRouterVariants)
                          : json::array()},
         {"default_effort", candidate.info.default_effort},
         {"active", active}});
  }
  return {{"models", models}, {"unavailable", search.unavailable}};
}

}  // namespace uagent
