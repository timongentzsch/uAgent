// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_PROVIDERS_H_
#define UAGENT_INCLUDE_PROVIDERS_H_
// Provider configuration and model discovery. The REPL consumes this interface;
// it does not parse provider JSON or mutate route environment variables itself.

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
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

struct ModelInfo {
  std::string id, default_effort;
  std::vector<std::string> efforts;
  int64_t context = 0;
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

inline std::vector<ModelRoute> LoadModelRoutes() {
  json providers = json::parse(EnvStr("UAGENT_PROVIDERS"), nullptr, false);
  std::vector<ModelRoute> routes;
  if (!providers.is_object()) return routes;
  for (const auto& [provider_name, provider] : providers.items()) {
    if (!provider.is_object() || provider_name.find('/') != std::string::npos) {
      continue;
    }
    std::string base_url = JsonString(provider, "base_url");
    if (base_url.empty() || !provider.contains("models") ||
        !provider["models"].is_object()) {
      continue;
    }
    base_url = StripTrailingSlashes(std::move(base_url));
    std::string api_key = JsonString(provider, "api_key", "sk-noop");
    int64_t context = JsonInt(provider, "context");
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
        routes.push_back(std::move(route));
      }
    }
  }
  return routes;
}

inline const ModelRoute* FindModelRoute(const std::vector<ModelRoute>& routes,
                                        const std::string& name) {
  for (const ModelRoute& route : routes) {
    if (route.name == name) return &route;
  }
  return nullptr;
}

inline void ExportRoute(const Api& api) {
  setenv("UAGENT_BASE_URL", api.base_url.c_str(), 1);
  setenv("UAGENT_API_KEY", api.api_key.c_str(), 1);
  setenv("UAGENT_MODEL", api.model.c_str(), 1);
  setenv("UAGENT_REASONING_EFFORT", api.reasoning_effort.c_str(), 1);
  setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
}

inline void ApplyRoute(Api& api, const ModelRoute& route) {
  api.base_url = route.base_url;
  api.api_key = route.api_key.empty() ? "sk-noop" : route.api_key;
  api.model = route.model;
  api.reasoning_effort = route.effort;
  api.ctx_window = route.context;
  api.native_tools = api.include_usage = api.parallel_tools = true;
  ExportRoute(api);
}

struct ProviderSetup {
  std::vector<ModelRoute> routes;
  std::string warning;
};

inline ProviderSetup ConfigureProvider(Api& api) {
  api.base_url = StripTrailingSlashes(EnvStr("UAGENT_BASE_URL"));
  api.api_key = EnvStr("UAGENT_API_KEY", "sk-noop");
  api.model = EnvStr("UAGENT_MODEL");
  api.reasoning_effort = EnvStr("UAGENT_REASONING_EFFORT");
  api.ctx_window = ContextWindow();

  ProviderSetup setup{LoadModelRoutes(), {}};
  if (const ModelRoute* route = FindModelRoute(setup.routes, api.model)) {
    ApplyRoute(api, *route);
  } else if (api.model.empty()) {  // no explicit model: restore the last /model
    ModelPreference preference = LoadModelPreference();
    if (preference.route) {
      if (const ModelRoute* route =
              FindModelRoute(setup.routes, preference.selection)) {
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

inline std::string SelectModel(Api& api, const std::vector<ModelRoute>& routes,
                               const std::string& name) {
  if (const ModelRoute* route = FindModelRoute(routes, name)) {
    ApplyRoute(api, *route);
    return route->name;
  }
  if (!OpenrouterUrl(api.base_url) || name.find('/') == std::string::npos) {
    return "";
  }
  if (api.model != name) {
    api.model = name;
    api.ctx_window = 0;
  }
  ExportRoute(api);
  return api.model;
}

inline std::optional<std::vector<ModelInfo>> ParseModels(const json& response,
                                                         std::string filter) {
  if (filter == "all") filter.clear();
  std::transform(filter.begin(), filter.end(), filter.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  if (!response.is_object() || !response.contains("data") ||
      !response["data"].is_array()) {
    return std::nullopt;
  }

  std::vector<ModelInfo> models;
  for (const json& model : response["data"]) {
    if (!model.is_object()) continue;
    std::string id = JsonString(model, "id");
    std::string lower = id;
    std::transform(
        lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (id.empty() ||
        (!filter.empty() && lower.find(filter) == std::string::npos)) {
      continue;
    }
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

inline std::optional<std::vector<ModelInfo>> QueryModels(Api& api,
                                                         std::string filter) {
  return ParseModels(api.Get("/models"), std::move(filter));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_PROVIDERS_H_
