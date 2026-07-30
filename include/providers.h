// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_PROVIDERS_H_
#define UAGENT_INCLUDE_PROVIDERS_H_
// Provider configuration and model discovery. The REPL consumes this interface;
// it does not parse provider JSON or mutate route environment variables itself.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <future>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

struct ModelRoute {
  std::string name, base_url, api_key, model, effort;
  int64_t context = 0;
};

struct NamedProvider {
  std::string name, base_url, api_key;
  int64_t context = 0;
};

struct ProviderCatalog {
  std::vector<NamedProvider> providers;
  std::vector<ModelRoute> models;
};

inline std::string NormalizeModelId(std::string model) {
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

struct ModelInfo {
  std::string id, default_effort;
  std::vector<std::string> efforts;
  int64_t context = 0;
};

struct ModelCandidate {
  std::string selection;
  ModelRoute route;
  ModelInfo info;
};

struct ModelSearch {
  std::vector<ModelCandidate> matches;
  std::vector<std::string> unavailable;
};

struct ModelPreference {
  std::string selection, base_url;
  bool route = false;
};

// One descriptor is all that is needed to add a zero-configuration provider.
// Explicit UAGENT_* settings and named routes still take precedence.
using ProviderUrlMatcher = bool (*)(std::string);
struct ProviderTemplate {
  const char* name;
  const char* base_url;
  const char* api_key_env;
  const char* model_env;
  const char* effort_env;
  const char* default_model;
  ProviderUrlMatcher matches_url;
};

inline constexpr ProviderTemplate kProviderTemplates[] = {{
    "openrouter",
    "https://openrouter.ai/api/v1",
    "OPENROUTER_API_KEY",
    "OPENROUTER_MODEL",
    "OPENROUTER_EFFORT",
    "openrouter/auto",
    OpenrouterUrl,
}};

inline const ProviderTemplate* FindProviderTemplate(const std::string& name) {
  for (const ProviderTemplate& provider : kProviderTemplates) {
    if (provider.name == name) return &provider;
  }
  return nullptr;
}

inline const ProviderTemplate* FindProviderTemplateForUrl(
    const std::string& url) {
  for (const ProviderTemplate& provider : kProviderTemplates) {
    if (provider.matches_url(url)) return &provider;
  }
  return nullptr;
}

inline bool ApplyProviderTemplate(Api& api, const ProviderTemplate& provider) {
  std::string api_key = EnvStr(provider.api_key_env);
  if (api_key.empty()) return false;
  api.base_url = provider.base_url;
  api.api_key = std::move(api_key);
  if (api.model.empty()) {
    api.model = EnvStr(provider.model_env, provider.default_model);
  }
  if (!getenv("UAGENT_REASONING_EFFORT")) {
    api.reasoning_effort = EnvStr(provider.effort_env);
  }
  return true;
}

inline std::string ModelPreferencePath() {
  return UagentDir(kConfigDir) + "/model-preference.json";
}

// A selection has to survive a round trip through the file and a header value.
inline bool PersistableSelection(const std::string& selection) {
  return !selection.empty() &&
         selection.find_first_of(std::string("\r\n", 3)) == std::string::npos;
}

inline ModelPreference LoadModelPreference() {
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

inline bool SaveModelPreference(const ModelPreference& preference,
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

inline constexpr const char* kReasoningEfforts[] = {
    "none", "minimal", "low", "medium", "high", "xhigh", "max"};

inline bool ValidEffort(const std::string& effort) {
  return effort.empty() ||
         std::find(std::begin(kReasoningEfforts), std::end(kReasoningEfforts),
                   effort) != std::end(kReasoningEfforts);
}

inline ProviderCatalog LoadProviderCatalog() {
  json providers = json::parse(EnvStr("UAGENT_PROVIDERS"), nullptr, false);
  ProviderCatalog catalog;
  if (!providers.is_object()) return catalog;
  for (const auto& [provider_name, provider] : providers.items()) {
    if (!provider.is_object() || provider_name.empty() ||
        provider_name == "all" ||
        provider_name.find('/') != std::string::npos) {
      continue;
    }
    std::string base_url = JsonString(provider, "base_url");
    if (base_url.empty()) continue;
    base_url = StripTrailingSlashes(std::move(base_url));
    std::string api_key = JsonString(provider, "api_key", "sk-noop");
    int64_t context = JsonInt(provider, "context");
    catalog.providers.push_back({provider_name, base_url, api_key, context});
    if (!provider.contains("models") || !provider["models"].is_object()) {
      continue;
    }
    for (const auto& [alias, spec] : provider["models"].items()) {
      if (alias.empty() || alias.find('/') != std::string::npos) continue;
      ModelRoute route;
      route.name = provider_name + "/" + alias;
      route.base_url = base_url;
      route.api_key = api_key;
      if (spec.is_string()) {
        route.model = spec.get<std::string>();
      } else if (spec.is_object()) {
        route.model = JsonString(spec, "id");
        route.effort = JsonString(spec, "effort");
        route.context = JsonInt(spec, "context", context);
      }
      if (!route.context) route.context = context;
      if (!route.model.empty() && ValidEffort(route.effort)) {
        catalog.models.push_back(std::move(route));
      }
    }
  }
  return catalog;
}

inline const NamedProvider* FindNamedProvider(
    const std::vector<NamedProvider>& providers, const std::string& name) {
  for (const NamedProvider& provider : providers) {
    if (provider.name == name) return &provider;
  }
  return nullptr;
}

inline void AddAvailableProviderTemplates(ProviderCatalog& catalog) {
  for (const ProviderTemplate& provider : kProviderTemplates) {
    std::string api_key = EnvStr(provider.api_key_env);
    if (api_key.empty() ||
        FindNamedProvider(catalog.providers, provider.name)) {
      continue;
    }
    catalog.providers.push_back(
        {provider.name, provider.base_url, std::move(api_key), 0});
  }
}

inline std::optional<ModelRoute> ResolveModelRoute(
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
                    provider->context};
}

inline void ExportRoute(const Api& api) {
  setenv("UAGENT_BASE_URL", api.base_url.c_str(), 1);
  setenv("UAGENT_MODEL", api.model.c_str(), 1);
  setenv("UAGENT_REASONING_EFFORT", api.reasoning_effort.c_str(), 1);
  setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
}

inline void ApplyRoute(Api& api, const ModelRoute& route) {
  api.base_url = route.base_url;
  api.api_key = route.api_key.empty() ? "sk-noop" : route.api_key;
  api.model = route.model;
  if (!route.effort.empty()) api.reasoning_effort = route.effort;
  api.ctx_window = route.context;
  api.native_tools = api.include_usage = api.parallel_tools =
      api.openrouter_web_search = true;
  ExportRoute(api);
}

struct ProviderSetup {
  std::vector<ModelRoute> routes;
  std::vector<NamedProvider> providers;
  std::string warning;
};

inline ProviderSetup ConfigureProvider(Api& api) {
  api.base_url = StripTrailingSlashes(EnvStr("UAGENT_BASE_URL"));
  api.api_key = EnvStr("UAGENT_API_KEY", "sk-noop");
  api.model = EnvStr("UAGENT_MODEL");
  api.reasoning_effort = EnvStr("UAGENT_REASONING_EFFORT");
  api.ctx_window = ContextWindow();

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
        ExportRoute(api);  // children inherit the resolved route
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

inline bool CanUseRawModel(const Api& api, std::string_view name) {
  return OpenrouterUrl(api.base_url) &&
         name.find('/') != std::string_view::npos;
}

inline std::string SelectModel(Api& api, const std::vector<ModelRoute>& routes,
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
  ExportRoute(api);
  return api.model;
}

inline std::optional<std::vector<ModelInfo>> ParseModels(const json& response) {
  if (!response.is_object() || !response.contains("data") ||
      !response["data"].is_array()) {
    return std::nullopt;
  }

  std::vector<ModelInfo> models;
  for (const json& model : response["data"]) {
    if (!model.is_object()) continue;
    std::string id = JsonString(model, "id");
    if (id.empty()) continue;
    ModelInfo info{std::move(id), {}, {}, JsonInt(model, "context_length")};
    if (model.contains("reasoning") && model["reasoning"].is_object()) {
      const json& reasoning = model["reasoning"];
      info.default_effort = JsonString(reasoning, "default_effort");
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

inline std::optional<std::vector<ModelInfo>> QueryModels(
    Api& api, bool abortable = false) {
  return ParseModels(api.Get("/models", abortable));
}

inline std::string NormalizeModelQuery(std::string query) {
  query = Trim(query);
  if (query == "all" || query == "*") return "";
  if (query.ends_with("/*")) query.pop_back();
  return query;
}

// Search every configured provider, regardless of the active route. Catalog
// requests run concurrently; configured aliases remain usable when a live
// catalog is unavailable.
inline ModelSearch SearchModels(const Api& api,
                                const std::vector<ModelRoute>& routes,
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
    std::string identity = route.base_url + "\n" + route.model;
    if (!selections.insert(route.name).second) continue;
    route_identities.insert(std::move(identity));
    ModelInfo info{route.model, {}, {}, route.context};
    result.matches.push_back({route.name, route, std::move(info)});
  }

  struct Catalog {
    std::string name, base_url, api_key;
    int64_t context = 0;
    bool named = false;
  };
  std::vector<Catalog> catalogs;
  for (const NamedProvider& provider : providers) {
    catalogs.push_back({provider.name, provider.base_url, provider.api_key,
                        provider.context, true});
  }
  bool active_is_named = std::any_of(
      catalogs.begin(), catalogs.end(),
      [&](const Catalog& source) { return source.base_url == api.base_url; });
  if (!active_is_named && !api.base_url.empty()) {
    catalogs.push_back({"", api.base_url, api.api_key, api.ctx_window, false});
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
      std::string identity = source.base_url + "\n" + info.id;
      if (!ContainsCaseInsensitive(selection, query) ||
          !selections.insert(selection).second ||
          !route_identities.insert(std::move(identity)).second) {
        continue;
      }
      int64_t context = info.context > 0 ? info.context : source.context;
      ModelRoute route{selection, source.base_url, source.api_key, info.id,
                       "",        context};
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

#endif  // UAGENT_INCLUDE_PROVIDERS_H_
