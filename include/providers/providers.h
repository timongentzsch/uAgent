// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_PROVIDERS_PROVIDERS_H_
#define UAGENT_INCLUDE_PROVIDERS_PROVIDERS_H_
// Provider configuration and model discovery.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/api/client.h"
#include "include/core/child_env.h"
#include "include/core/json.h"

namespace uagent {

struct ProviderCatalog {
  std::vector<Endpoint> providers;
  std::vector<Route> models;
};

struct ModelInfo {
  std::string id, default_effort;
  std::vector<std::string> efforts;
  int64_t context = 0;
  json input_modalities = nullptr;
  // The catalog's display name; never used as an id.
  std::string name = "";
  json features = nullptr;
};

// route.route_name is the selection that chooses it.
struct ModelCandidate {
  Route route;
  ModelInfo info;
};

struct ModelSearch {
  std::vector<ModelCandidate> matches;
  std::vector<std::string> unavailable;
  // Live catalogs queried (routes always count as available).
  size_t queried = 0;
};

json ModelCatalogue(Api& api, const std::vector<Route>& routes,
                    const std::vector<Endpoint>& providers,
                    const std::string& query = "all");

struct ProviderSetup {
  std::vector<Route> routes;
  std::vector<Endpoint> providers;
  std::string warning;
};

// One grammar for every model-valued setting: [endpoint/]model[:effort].
// Only a known effort is peeled from the right, so ids like
// `deepseek-chat:free` stay intact.
struct ModelSelection {
  std::string base;
  std::string effort;
};

ModelSelection ParseModelSelection(const std::string& selection);

// Where a side model runs: the parent's route unless the selection names one
// that resolves. `none` says why no model serves the route.
struct RouteResolution {
  Route route;
  std::string selection, none;
};

// The route variables a process started for this route reads: endpoint,
// model, features and options. Never the API key.
EnvironmentOverrides RouteEnvironment(const Route& route);

RouteResolution ResolveSideRoute(const Api& api,
                                 const std::vector<Route>& routes,
                                 const std::vector<Endpoint>& providers,
                                 const std::string& requested);

// The route that reads images when the conversation's model cannot:
// `setting` if set, else the same endpoint's first model that accepts them.
RouteResolution ResolveImageRoute(const Api& api,
                                  const std::vector<Route>& routes,
                                  const std::vector<Endpoint>& providers,
                                  const std::string& setting);

// Where the permission review asks: `setting` if set, else an endpoint's
// decision model, the conversation's endpoint first. A chat model never
// stands in.
RouteResolution ResolveReviewRoute(const Api& api,
                                   const std::vector<Endpoint>& providers,
                                   const std::string& setting);

bool ValidEffort(const std::string& effort);
bool SupportsReasoningEffort(const Api& api, std::string_view effort);
// The providers of the settings, with the routes their aliases name.
ProviderCatalog LoadProviderCatalog();
const Endpoint* FindEndpoint(const std::vector<Endpoint>& providers,
                             const std::string& name);
// Applies what a well-known host declares; false for an unknown address.
bool DeclareHost(Endpoint& endpoint);
// Whether an endpoint has a key of its own, not the placeholder for none.
bool HasKey(const Endpoint& endpoint);
std::optional<Route> ResolveModelRoute(const std::vector<Route>& routes,
                                       const std::vector<Endpoint>& providers,
                                       const std::string& selection);
// The selection that reproduces a route: provider/model[:effort].
std::string RouteSelection(const Route& route,
                           const std::vector<Endpoint>& providers);
// The same, by the alias the route was chosen by where it has one, so a
// session handed this selection resolves the same route.
std::string ChosenSelection(const Api& api,
                            const std::vector<Endpoint>& providers);
std::string ProviderScope(const Endpoint& endpoint,
                          const std::vector<Endpoint>& providers);
std::string ComposeSelection(const std::string& scope, const std::string& model,
                             const std::string& effort);
void ApplySelectionPolicy(Api& api, const ModelSelection& selection);
// Makes `route` the one `api` talks to and starts its capabilities afresh.
void ApplyRoute(Api& api, const Route& route);
void ActivateRoute(Api& api);
ProviderSetup ConfigureProvider(Api& api);
// Discover a missing model or context limit; a failed limit probe is advisory.
bool ProbeModel(Api& api, bool discover_efforts = false);
bool CanUseRawModel(const Api& api, std::string_view name);
std::string SelectModel(Api& api, const std::vector<Route>& routes,
                        const std::vector<Endpoint>& providers,
                        const std::string& name);
int64_t CatalogContextLength(const json& model);
std::optional<std::vector<ModelInfo>> ParseModels(const json& response,
                                                  bool free_only = false);
// The catalogue the last successful query left, while at most a day old.
std::optional<std::vector<ModelInfo>> CachedModels(const Api& api);
std::optional<std::vector<ModelInfo>> QueryModels(Api& api,
                                                  bool abortable = false,
                                                  int64_t timeout_s = 15);
std::string NormalizeModelQuery(std::string query);
// Exact substring first, then one that unifies spaces, underscores and
// hyphens. Selection stays id-based; only matching is lenient.
bool MatchesModelQuery(const std::string& haystack, const std::string& query);
ModelSearch SearchModels(const Api& api, const std::vector<Route>& routes,
                         const std::vector<Endpoint>& providers,
                         std::string query);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_PROVIDERS_PROVIDERS_H_
