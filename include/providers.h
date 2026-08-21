// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_PROVIDERS_H_
#define UAGENT_INCLUDE_PROVIDERS_H_
// Provider configuration and model discovery. The REPL consumes this interface;
// it does not parse provider JSON or mutate route environment variables itself.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/api.h"
#include "include/core/json.h"

namespace uagent {

struct ModelRoute {
  std::string name, base_url, api_key, model, effort;
  int64_t context = 0;
  ProviderProtocol protocol = ProviderProtocol::kOpenAi;
};

struct NamedProvider {
  std::string name, base_url, api_key;
  int64_t context = 0;
  ProviderProtocol protocol = ProviderProtocol::kOpenAi;
};

struct ProviderCatalog {
  std::vector<NamedProvider> providers;
  std::vector<ModelRoute> models;
};

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

using ProviderUrlMatcher = bool (*)(std::string);
struct ProviderTemplate {
  const char* name;
  const char* base_url;
  const char* api_key_env;
  const char* model_env;
  const char* effort_env;
  const char* default_model;
  ProviderUrlMatcher matches_url;
  ProviderProtocol protocol;
};

struct ProviderSetup {
  std::vector<ModelRoute> routes;
  std::vector<NamedProvider> providers;
  std::string warning;
};

inline constexpr const char* kReasoningEfforts[] = {
    "none", "minimal", "low", "medium", "high", "xhigh", "max"};

// One selection grammar for every model-valued setting:
//
//   [provider/]model[:variant][:effort]
//
// Suffixes are peeled from the right and classified against two closed sets —
// kReasoningEfforts and kOpenRouterVariants — so their order does not matter.
// Peeling stops at the first unrecognized suffix, which stays in the model id:
// that is what keeps OpenRouter ids like `deepseek-chat:free` intact without an
// escape syntax. `base` is what the route resolvers still see, unchanged.
struct ModelSelection {
  std::string base;
  std::string variant;
  std::string effort;
};

ModelSelection ParseModelSelection(std::string selection);

// Where a side model runs: the parent's route unless the selection names one
// that resolves. `unresolved` marks a named route that matched nothing, which
// only the execution paths treat as an error. Shared by the subagent, vision
// and memory-extraction paths so one selection means one thing.
struct SideRoute {
  std::string selection, model, base_url, api_key, effort, variant;
  int64_t context = 0;
  ProviderProtocol protocol = ProviderProtocol::kOpenAi;
  bool unresolved = false;
};

SideRoute ResolveSideRoute(const Api& api,
                           const std::vector<ModelRoute>& routes,
                           const std::vector<NamedProvider>& providers,
                           const std::string& requested);

std::string NormalizeModelId(std::string model);
const ProviderTemplate* FindProviderTemplateForUrl(const std::string& url);
bool ApplyProviderTemplate(Api& api, const ProviderTemplate& provider);
std::string ModelPreferencePath();
bool PersistableSelection(const std::string& selection);
ModelPreference LoadModelPreference();
bool SaveModelPreference(const ModelPreference& preference, std::string& error);
bool ValidEffort(const std::string& effort);
ProviderCatalog LoadProviderCatalog();
// The catalog a side model should resolve against: configured providers plus
// the built-in templates whose API key is present, so `openrouter/<id>` means
// the same thing to every consumer of the selection schema.
ProviderCatalog SessionProviderCatalog();
const NamedProvider* FindNamedProvider(
    const std::vector<NamedProvider>& providers, const std::string& name);
void AddAvailableProviderTemplates(ProviderCatalog& catalog);
std::optional<ModelRoute> ResolveModelRoute(
    const std::vector<ModelRoute>& routes,
    const std::vector<NamedProvider>& providers, const std::string& selection);
// The selection that would reproduce the active route in the shared schema,
// [provider/]model[:variant][:effort] — the same string the user could pass
// to --model. The provider scope is omitted when no configured provider or
// template serves this base URL.
std::string RouteSelection(const Api& api,
                           const std::vector<NamedProvider>& providers);
std::string RouteSelection(const SideRoute& route,
                           const std::vector<NamedProvider>& providers);
void ApplyRoute(Api& api, const ModelRoute& route);
// The in-process counterpart for side models: same fields as ApplyRoute
// plus the routing variant the selection asked for.
void ApplySideRoute(Api& api, const SideRoute& route);
void ActivateRoute(Api& api);
ProviderSetup ConfigureProvider(Api& api);
bool CanUseRawModel(const Api& api, std::string_view name);
std::string SelectModel(Api& api, const std::vector<ModelRoute>& routes,
                        const std::vector<NamedProvider>& providers,
                        const std::string& name);
int64_t CatalogContextLength(const json& model);
std::optional<std::vector<ModelInfo>> ParseModels(const json& response);
std::optional<std::vector<ModelInfo>> QueryModels(Api& api,
                                                  bool abortable = false);
std::string NormalizeModelQuery(std::string query);
ModelSearch SearchModels(const Api& api, const std::vector<ModelRoute>& routes,
                         const std::vector<NamedProvider>& providers,
                         std::string query);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_PROVIDERS_H_
