// Copyright 2026 Timon Gentzsch

#include "include/providers.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr ProviderTemplate kProviderTemplates[] = {{
    "openrouter",
    "https://openrouter.ai/api/v1",
    "OPENROUTER_API_KEY",
    "OPENROUTER_MODEL",
    "OPENROUTER_EFFORT",
    "openrouter/auto",
    OpenrouterUrl,
    ProviderProtocol::kOpenRouter,
}};

}  // namespace

std::string NormalizeModelId(std::string model) {
  model = Trim(model);
  bool separator = false;
  std::string normalized;
  normalized.reserve(model.size());
  for (unsigned char value : model) {
    if (std::isspace(value)) {
      separator = !normalized.empty();
      continue;
    }
    if (separator && normalized.back() != '-') normalized += '-';
    separator = false;
    normalized += static_cast<char>(value);
  }
  return normalized;
}

const ProviderTemplate* FindProviderTemplate(const std::string& name) {
  for (const ProviderTemplate& provider : kProviderTemplates) {
    if (provider.name == name) return &provider;
  }
  return nullptr;
}

const ProviderTemplate* FindProviderTemplateForUrl(const std::string& url) {
  for (const ProviderTemplate& provider : kProviderTemplates) {
    if (provider.matches_url(url)) return &provider;
  }
  return nullptr;
}

bool ApplyProviderTemplate(Api& api, const ProviderTemplate& provider) {
  std::string api_key = EnvStr(provider.api_key_env);
  if (api_key.empty()) return false;
  api.base_url = provider.base_url;
  api.api_key = std::move(api_key);
  api.capabilities = CapabilitiesForRoute(provider.protocol, api.base_url);
  if (api.model.empty()) {
    api.model = EnvStr(provider.model_env, provider.default_model);
  }
  if (!getenv("UAGENT_REASONING_EFFORT")) {
    api.reasoning_effort = EnvStr(provider.effort_env);
  }
  return true;
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
  return AtomicWriteFile(ModelPreferencePath(), JsonDump(saved, 2) + "\n", 0600,
                         /*preserve_mode=*/false, error);
}

ModelSelection ParseModelSelection(std::string selection) {
  ModelSelection parsed;
  parsed.base = Trim(std::move(selection));
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

ProviderCatalog LoadProviderCatalog() {
  json providers = json::parse(EnvStr("UAGENT_PROVIDERS"), nullptr, false);
  ProviderCatalog catalog;
  if (!providers.is_object()) return catalog;
  for (const auto& [provider_name, provider] : providers.items()) {
    if (!provider.is_object() || provider_name.empty() ||
        provider_name == "all" ||
        provider_name.find('/') != std::string::npos) {
      continue;
    }
    std::string base_url = JsonValue(provider, "base_url", "");
    if (base_url.empty()) continue;
    base_url = StripTrailingSlashes(std::move(base_url));
    std::string api_key = JsonValue(provider, "api_key", "sk-noop");
    int64_t context = JsonValue(provider, "context", int64_t{0});
    ProviderProtocol protocol =
        ParseProviderProtocol(JsonValue(provider, "protocol", "openai"));
    catalog.providers.push_back(
        {provider_name, base_url, api_key, context, protocol});
    if (!provider.contains("models") || !provider["models"].is_object()) {
      continue;
    }
    for (const auto& [alias, spec] : provider["models"].items()) {
      if (alias.empty() || alias.find('/') != std::string::npos) continue;
      ModelRoute route;
      route.name = provider_name + "/" + alias;
      route.base_url = base_url;
      route.api_key = api_key;
      route.protocol = protocol;
      if (spec.is_string()) {
        route.model = spec.get<std::string>();
      } else if (spec.is_object()) {
        route.model = JsonValue(spec, "id", "");
        route.effort = JsonValue(spec, "effort", "");
        route.context = JsonValue(spec, "context", context);
      }
      if (!route.context) route.context = context;
      if (!route.model.empty() && ValidEffort(route.effort)) {
        catalog.models.push_back(std::move(route));
      }
    }
  }
  return catalog;
}

const NamedProvider* FindNamedProvider(
    const std::vector<NamedProvider>& providers, const std::string& name) {
  for (const NamedProvider& provider : providers) {
    if (provider.name == name) return &provider;
  }
  return nullptr;
}

void AddAvailableProviderTemplates(ProviderCatalog& catalog) {
  for (const ProviderTemplate& provider : kProviderTemplates) {
    std::string api_key = EnvStr(provider.api_key_env);
    if (api_key.empty() ||
        FindNamedProvider(catalog.providers, provider.name)) {
      continue;
    }
    catalog.providers.push_back({provider.name, provider.base_url,
                                 std::move(api_key), 0, provider.protocol});
  }
}

ProviderCatalog SessionProviderCatalog() {
  ProviderCatalog catalog = LoadProviderCatalog();
  AddAvailableProviderTemplates(catalog);
  return catalog;
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
  return ModelRoute{selection,
                    provider->base_url,
                    provider->api_key,
                    selection.substr(slash + 1),
                    "",
                    provider->context,
                    provider->protocol};
}

namespace {

// Two selections naming the same endpoint, model and protocol are the same
// route however they were spelled; only one of them is offered.
std::string RouteIdentity(const std::string& base_url, const std::string& model,
                          ProviderProtocol protocol) {
  return base_url + "\n" + model + "\n" + ProviderProtocolName(protocol);
}

void ExportRoute(const Api& api) {
  setenv("UAGENT_BASE_URL", api.base_url.c_str(), 1);
  setenv("UAGENT_MODEL", api.model.c_str(), 1);
  setenv("UAGENT_REASONING_EFFORT", api.reasoning_effort.c_str(), 1);
  setenv("UAGENT_OPENROUTER_VARIANT", api.config.openrouter_variant.c_str(), 1);
  setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
}

void ResetRouteCapabilities(Api& api) {
  api.capabilities =
      CapabilitiesForRoute(api.capabilities.protocol, api.base_url);
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
  if (!parsed.base.empty()) {
    if (std::optional<ModelRoute> route =
            ResolveModelRoute(routes, providers, parsed.base)) {
      resolved.base_url = route->base_url;
      resolved.api_key = route->api_key.empty() ? "sk-noop" : route->api_key;
      resolved.model = route->model;
      if (!route->effort.empty()) resolved.effort = route->effort;
      resolved.context = route->context;
      resolved.protocol = route->protocol;
    } else {
      // A bare model id on the parent's provider also lands here; only the
      // execution paths decide whether that is usable.
      resolved.unresolved = true;
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
  api.api_key = route.api_key.empty() ? "sk-noop" : route.api_key;
  api.model = route.model;
  if (!route.effort.empty()) api.reasoning_effort = route.effort;
  api.ctx_window = route.context;
  api.capabilities = CapabilitiesForRoute(route.protocol, route.base_url);
}

namespace {

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

}  // namespace

std::string RouteSelection(const Api& api,
                           const std::vector<NamedProvider>& providers) {
  // CatalogModel strips a routing variant the request appends, so the suffixes
  // are added once and in schema order.
  return ComposeSelection(ProviderScope(api.base_url, providers),
                          api.CatalogModel(),
                          api.capabilities.model_variants
                              ? api.config.openrouter_variant
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
  api.api_key = route.api_key.empty() ? "sk-noop" : route.api_key;
  api.model = route.model;
  api.reasoning_effort = route.effort;
  api.ctx_window = route.context;
  api.capabilities = CapabilitiesForRoute(route.protocol, route.base_url);
  api.config.openrouter_variant = route.variant;
}

void ActivateRoute(Api& api) {
  ResetRouteCapabilities(api);
  ExportRoute(api);
}

ProviderSetup ConfigureProvider(Api& api) {
  api.base_url = StripTrailingSlashes(EnvStr("UAGENT_BASE_URL"));
  api.api_key = EnvStr("UAGENT_API_KEY", "sk-noop");
  api.model = EnvStr("UAGENT_MODEL");
  api.reasoning_effort = EnvStr("UAGENT_REASONING_EFFORT");
  api.ctx_window = ContextWindow();
  std::string compatible = EnvStr("UAGENT_OPENROUTER_COMPATIBLE");
  ProviderProtocol protocol =
      compatible.empty()
          ? (OpenrouterUrl(api.base_url) ? ProviderProtocol::kOpenRouter
                                         : ProviderProtocol::kOpenAi)
          : (compatible == "1" ? ProviderProtocol::kOpenRouter
                               : ProviderProtocol::kOpenAi);
  api.capabilities = CapabilitiesForRoute(protocol, api.base_url);

  ProviderCatalog catalog = LoadProviderCatalog();
  AddAvailableProviderTemplates(catalog);
  ProviderSetup setup{
      std::move(catalog.models), std::move(catalog.providers), {}};
  if (std::optional<ModelRoute> route =
          ResolveModelRoute(setup.routes, setup.providers, api.model)) {
    ApplyRoute(api, *route);
  } else if (api.model.empty()) {  // no explicit model: restore the last /model
    ModelPreference preference = LoadModelPreference();
    if (preference.route) {
      if (std::optional<ModelRoute> route = ResolveModelRoute(
              setup.routes, setup.providers, preference.selection)) {
        ApplyRoute(api, *route);
      }
    } else if (!preference.selection.empty()) {
      bool same_provider = api.base_url == preference.base_url;
      if (api.base_url.empty()) {
        const ProviderTemplate* provider =
            FindProviderTemplateForUrl(preference.base_url);
        same_provider = provider && !EnvStr(provider->api_key_env).empty();
      }
      if (same_provider) api.model = preference.selection;
    }
  }
  if (api.base_url.empty()) {
    for (const ProviderTemplate& provider : kProviderTemplates) {
      if (ApplyProviderTemplate(api, provider)) {
        break;
      }
    }
  }
  if (!ValidEffort(api.reasoning_effort)) {
    setup.warning =
        "ignoring invalid reasoning effort: " + api.reasoning_effort;
    api.reasoning_effort.clear();
    setenv("UAGENT_REASONING_EFFORT", "", 1);
  }
  return setup;
}

bool CanUseRawModel(const Api& api, std::string_view name) {
  return api.capabilities.raw_slash_models &&
         name.find('/') != std::string_view::npos;
}

std::string SelectModel(Api& api, const std::vector<ModelRoute>& routes,
                        const std::vector<NamedProvider>& providers,
                        const std::string& name) {
  if (std::optional<ModelRoute> route =
          ResolveModelRoute(routes, providers, name)) {
    ApplyRoute(api, *route);
    return route->name;
  }
  if (!CanUseRawModel(api, name)) return "";
  if (api.model != name) {
    api.model = name;
    api.ctx_window = 0;
  }
  return api.model;
}

int64_t CatalogContextLength(const json& model) {
  if (int64_t context = JsonValue(model, "context_length", int64_t{0})) {
    return context;
  }
  if (int64_t context = JsonValue(model, "max_model_len", int64_t{0})) {
    return context;
  }
  if (model.contains("meta") && model["meta"].is_object()) {
    return JsonValue(model["meta"], "n_ctx_train", int64_t{0});
  }
  return 0;
}

std::optional<std::vector<ModelInfo>> ParseModels(const json& response) {
  if (!response.is_object() || !response.contains("data") ||
      !response["data"].is_array()) {
    return std::nullopt;
  }

  std::vector<ModelInfo> models;
  for (const json& model : response["data"]) {
    if (!model.is_object()) continue;
    std::string id = JsonValue(model, "id", "");
    if (id.empty()) continue;
    ModelInfo info{std::move(id), {}, {}, CatalogContextLength(model)};
    if (model.contains("reasoning") && model["reasoning"].is_object()) {
      const json& reasoning = model["reasoning"];
      info.default_effort = JsonValue(reasoning, "default_effort", "");
      if (reasoning.contains("supported_efforts") &&
          reasoning["supported_efforts"].is_array()) {
        for (const json& effort : reasoning["supported_efforts"]) {
          if (effort.is_string()) {
            info.efforts.push_back(effort.get<std::string>());
          }
        }
      }
    }
    models.push_back(std::move(info));
  }
  std::sort(models.begin(), models.end(),
            [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
  return models;
}

std::optional<std::vector<ModelInfo>> QueryModels(Api& api, bool abortable) {
  return ParseModels(api.Get("/models", abortable));
}

std::string NormalizeModelQuery(std::string query) {
  query = Trim(query);
  if (query == "all" || query == "*") return "";
  if (query.ends_with("/*")) query.pop_back();
  return query;
}

ModelSearch SearchModels(const Api& api, const std::vector<ModelRoute>& routes,
                         const std::vector<NamedProvider>& providers,
                         std::string query) {
  query = NormalizeModelQuery(std::move(query));
  ModelSearch result;
  std::set<std::string> selections;
  std::set<std::string> route_identities;
  for (const ModelRoute& route : routes) {
    if (!ContainsCaseInsensitive(route.name + " " + route.model, query)) {
      continue;
    }
    std::string identity =
        RouteIdentity(route.base_url, route.model, route.protocol);
    if (!selections.insert(route.name).second) continue;
    route_identities.insert(std::move(identity));
    ModelInfo info{route.model, {}, {}, route.context};
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
                        api.capabilities.protocol});
  }

  using CatalogModels = std::optional<std::vector<ModelInfo>>;
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
            CapabilitiesForRoute(source.protocol, source.base_url);
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
          RouteIdentity(source.base_url, info.id, source.protocol);
      if (!ContainsCaseInsensitive(selection, query) ||
          !selections.insert(selection).second ||
          !route_identities.insert(std::move(identity)).second) {
        continue;
      }
      int64_t context = info.context > 0 ? info.context : source.context;
      ModelRoute route{selection, source.base_url, source.api_key, info.id,
                       "",        context,         source.protocol};
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

}  // namespace uagent
