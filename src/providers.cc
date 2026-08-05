// Copyright 2026 Timon Gentzsch

#include "include/providers.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
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
    true,
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
  api.openrouter_compatible = provider.openrouter_compatible;
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
    bool openrouter_compatible =
        JsonValue(provider, "protocol", "openai") == "openrouter";
    catalog.providers.push_back(
        {provider_name, base_url, api_key, context, openrouter_compatible});
    if (!provider.contains("models") || !provider["models"].is_object()) {
      continue;
    }
    for (const auto& [alias, spec] : provider["models"].items()) {
      if (alias.empty() || alias.find('/') != std::string::npos) continue;
      ModelRoute route;
      route.name = provider_name + "/" + alias;
      route.base_url = base_url;
      route.api_key = api_key;
      route.openrouter_compatible = openrouter_compatible;
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
                                 std::move(api_key), 0,
                                 provider.openrouter_compatible});
  }
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
                    provider->openrouter_compatible};
}

void ExportRoute(const Api& api) {
  setenv("UAGENT_BASE_URL", api.base_url.c_str(), 1);
  setenv("UAGENT_MODEL", api.model.c_str(), 1);
  setenv("UAGENT_REASONING_EFFORT", api.reasoning_effort.c_str(), 1);
  setenv("UAGENT_OPENROUTER_VARIANT", api.config.openrouter_variant.c_str(), 1);
  setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
}

void ResetRouteCapabilities(Api& api) {
  api.native_tools = true;
  api.include_usage = true;
  api.parallel_tools = true;
  api.openrouter_web_search = api.config.web_search_backend != "off" &&
                              api.config.web_search_backend != "responses" &&
                              api.openrouter_compatible;
  api.image_input = true;
  api.route_certified = false;
}

void ApplyRoute(Api& api, const ModelRoute& route) {
  api.base_url = route.base_url;
  api.api_key = route.api_key.empty() ? "sk-noop" : route.api_key;
  api.model = route.model;
  if (!route.effort.empty()) api.reasoning_effort = route.effort;
  api.ctx_window = route.context;
  api.openrouter_compatible = route.openrouter_compatible;
}

std::string RouteProfilesPath() {
  return UagentDir(kConfigDir) + "/routes.json";
}

std::string RouteProfileKey(const Api& api) {
  return RouteKey(api.base_url, api.config.openrouter_provider,
                  api.RequestModel(), api.reasoning_effort);
}

namespace {

json LoadRouteProfiles() {
  std::ifstream input(RouteProfilesPath());
  json profiles = json::parse(input, nullptr, false);
  if (!profiles.is_object() || JsonValue(profiles, "schema", 0) != 3 ||
      !profiles.contains("routes") || !profiles["routes"].is_object()) {
    return json::object();
  }
  return profiles;
}

std::string RouteFamilyKey(const Api& api) {
  return RouteKey(api.base_url, api.config.openrouter_provider,
                  api.RequestModel(), "");
}

json CertifiedProfile(const json& profiles, const std::string& key) {
  if (!profiles.contains("routes") || !profiles["routes"].is_object()) {
    return json::object();
  }
  auto found = profiles["routes"].find(key);
  if (found == profiles["routes"].end() || !found->is_object()) {
    return json::object();
  }
  constexpr int64_t kProfileLifetimeSeconds = 30 * 24 * 60 * 60;
  int64_t certified = JsonValue(*found, "certified_at_unix", int64_t{0});
  int64_t now = static_cast<int64_t>(std::time(nullptr));
  int64_t samples = JsonValue(*found, "samples", int64_t{0});
  if (!JsonValue(*found, "certified", false) || samples <= 0 ||
      JsonValue(*found, "passing_samples", int64_t{0}) != samples ||
      JsonValue(*found, "pass_rate", 0.0) < 1.0 ||
      JsonValue(*found, "invalidated_at_unix", int64_t{0}) > 0 ||
      certified <= 0 || certified > now ||
      now - certified > kProfileLifetimeSeconds) {
    return json::object();
  }
  return *found;
}

bool ProfileMatchesConfig(const json& profile, const Api& api) {
  return JsonValue(profile, "openrouter_compatible",
                   OpenrouterUrl(api.base_url)) == api.openrouter_compatible &&
         JsonValue(profile, "memory_enabled", true) ==
             api.config.memory_enabled &&
         JsonValue(profile, "memory_generate", true) ==
             api.config.memory_generate;
}

}  // namespace

json RouteProfile(const Api& api) {
  json profiles = LoadRouteProfiles();
  if (profiles.empty()) return json::object();
  json profile = CertifiedProfile(profiles, RouteProfileKey(api));
  if (!profile.empty() && !ProfileMatchesConfig(profile, api)) {
    return json::object();
  }
  return profile;
}

json ApplyRouteProfile(Api& api) {
  if (api.reasoning_effort.empty()) {
    json profiles = LoadRouteProfiles();
    if (profiles.contains("recommendations") &&
        profiles["recommendations"].is_object()) {
      auto recommendation =
          profiles["recommendations"].find(RouteFamilyKey(api));
      if (recommendation != profiles["recommendations"].end() &&
          recommendation->is_object()) {
        std::string effort = JsonValue(*recommendation, "effort", "");
        std::string route = JsonValue(*recommendation, "route", "");
        std::string expected =
            RouteKey(api.base_url, api.config.openrouter_provider,
                     api.RequestModel(), effort);
        json profile = CertifiedProfile(profiles, route);
        if (!effort.empty() && ValidEffort(effort) && route == expected &&
            !profile.empty() && ProfileMatchesConfig(profile, api)) {
          api.reasoning_effort = std::move(effort);
          ExportRoute(api);
        }
      }
    }
  }
  json profile = RouteProfile(api);
  if (profile.empty()) return profile;
  api.route_certified = profile.contains("checkpoint_mode") ||
                        profile.contains("parallel_hint_support") ||
                        profile.contains("image_support");
  if (api.config.checkpoint_mode == "apply" &&
      JsonValue(profile, "checkpoint_mode", "apply") == "shadow") {
    api.config.checkpoint_mode = "shadow";
  }
  if (profile.contains("parallel_hint_support") &&
      profile["parallel_hint_support"].is_boolean() &&
      !profile["parallel_hint_support"].get<bool>()) {
    api.parallel_tools = false;
  }
  if (profile.contains("image_support") &&
      profile["image_support"].is_boolean() &&
      !profile["image_support"].get<bool>()) {
    api.image_input = false;
  }
  return profile;
}

json ActivateRoute(Api& api, const std::string& checkpoint_mode) {
  api.config.checkpoint_mode = checkpoint_mode;
  ResetRouteCapabilities(api);
  json profile = ApplyRouteProfile(api);
  ExportRoute(api);
  return profile;
}

bool InvalidateRouteProfile(const Api& api, const std::string& feature) {
  json profiles = LoadRouteProfiles();
  if (profiles.empty()) return false;
  auto route = profiles["routes"].find(RouteProfileKey(api));
  if (route == profiles["routes"].end() || !route->is_object()) return false;
  (*route)["invalidated_at_unix"] = static_cast<int64_t>(std::time(nullptr));
  (*route)["invalidated_feature"] = feature;
  if (profiles.contains("recommendations") &&
      profiles["recommendations"].is_object()) {
    profiles["recommendations"].erase(RouteFamilyKey(api));
  }
  std::string error;
  return AtomicWriteFile(RouteProfilesPath(), JsonDump(profiles, 2) + "\n",
                         0600, /*preserve_mode=*/true, error);
}

ProviderSetup ConfigureProvider(Api& api) {
  api.base_url = StripTrailingSlashes(EnvStr("UAGENT_BASE_URL"));
  api.api_key = EnvStr("UAGENT_API_KEY", "sk-noop");
  api.model = EnvStr("UAGENT_MODEL");
  api.reasoning_effort = EnvStr("UAGENT_REASONING_EFFORT");
  api.ctx_window = ContextWindow();
  std::string compatible = EnvStr("UAGENT_OPENROUTER_COMPATIBLE");
  api.openrouter_compatible =
      compatible.empty() ? OpenrouterUrl(api.base_url) : compatible == "1";

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
  return api.openrouter_compatible && name.find('/') != std::string_view::npos;
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
    ModelInfo info{
        std::move(id), {}, {}, JsonValue(model, "context_length", int64_t{0})};
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
        route.base_url + "\n" + route.model + "\n" +
        (route.openrouter_compatible ? "openrouter" : "openai");
    if (!selections.insert(route.name).second) continue;
    route_identities.insert(std::move(identity));
    ModelInfo info{route.model, {}, {}, route.context};
    result.matches.push_back({route.name, route, std::move(info)});
  }

  struct Catalog {
    std::string name, base_url, api_key;
    int64_t context = 0;
    bool named = false;
    bool openrouter_compatible = false;
  };
  std::vector<Catalog> catalogs;
  catalogs.reserve(providers.size() + 1);
  for (const NamedProvider& provider : providers) {
    catalogs.push_back({provider.name, provider.base_url, provider.api_key,
                        provider.context, true,
                        provider.openrouter_compatible});
  }
  bool active_is_named = std::any_of(
      catalogs.begin(), catalogs.end(),
      [&](const Catalog& source) { return source.base_url == api.base_url; });
  if (!active_is_named && !api.base_url.empty()) {
    catalogs.push_back({"", api.base_url, api.api_key, api.ctx_window, false,
                        api.openrouter_compatible});
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
        const Catalog& source = catalogs[index];
        Api catalog_api(api.config);
        catalog_api.base_url = source.base_url;
        catalog_api.api_key = source.api_key;
        catalog_api.openrouter_compatible = source.openrouter_compatible;
        responses[index] = QueryModels(catalog_api, /*abortable=*/true);
      }
    }));
  }
  for (auto& worker : workers) worker.get();
  for (size_t i = 0; i < catalogs.size(); ++i) {
    const Catalog& source = catalogs[i];
    CatalogModels models = std::move(responses[i]);
    if (!models) {
      result.unavailable.push_back(
          source.name.empty() ? UrlHost(source.base_url) : source.name);
      continue;
    }
    for (ModelInfo& info : *models) {
      std::string selection =
          source.named ? source.name + "/" + info.id : info.id;
      std::string identity =
          source.base_url + "\n" + info.id + "\n" +
          (source.openrouter_compatible ? "openrouter" : "openai");
      if (!ContainsCaseInsensitive(selection, query) ||
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
                       source.openrouter_compatible};
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
