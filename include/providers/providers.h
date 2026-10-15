// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_PROVIDERS_PROVIDERS_H_
#define UAGENT_INCLUDE_PROVIDERS_PROVIDERS_H_
// Provider configuration and model discovery. The REPL consumes this interface;
// it does not parse provider JSON or mutate route environment variables itself.

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
  // Display name from the catalog (`name`); empty when the provider sends
  // none. Never used as an id — search and selection stay id-based.
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

// One selection grammar for every model-valued setting:
//
//   [endpoint/]model[:effort]
//
// The effort is peeled from the right when it is one of kReasoningEfforts.
// Any other suffix stays in the model id, which is what keeps ids like
// `deepseek-chat:free` and an endpoint's routing suffixes (`:nitro`) intact
// without an escape syntax. `base` is what the route resolvers still see.
struct ModelSelection {
  std::string base;
  std::string effort;
};

ModelSelection ParseModelSelection(const std::string& selection);

// Where a side model runs: the parent's route unless the selection names one
// that resolves. `unresolved` marks a named route that matched nothing, which
// only the execution paths treat as an error. `none` says why no model serves
// the route, when none does. Shared by the title, subagent, vision and
// memory-extraction paths so one selection means one thing.
struct RouteResolution {
  Route route;
  std::string selection, none;
  bool unresolved = false;
};

// The route variables a process started for this route reads: endpoint,
// model, features and options. Never the API key.
EnvironmentOverrides RouteEnvironment(const Route& route);

RouteResolution ResolveSideRoute(const Api& api,
                                 const std::vector<Route>& routes,
                                 const std::vector<Endpoint>& providers,
                                 const std::string& requested);

// The route that reads images when the conversation's model cannot: `setting`
// (image.model) if set, else the first model of the same endpoint that accepts
// them. `none` says why when there is none.
RouteResolution ResolveImageRoute(const Api& api,
                                  const std::vector<Route>& routes,
                                  const std::vector<Endpoint>& providers,
                                  const std::string& setting);

std::string NormalizeModelId(std::string model);
bool ValidEffort(const std::string& effort);
bool SupportsReasoningEffort(const Api& api, std::string_view effort);
// The providers of the settings plus the well-known endpoints whose key is
// set, with the routes their aliases name.
ProviderCatalog LoadProviderCatalog();
// Every consumer of the selection schema reads the same catalogue, so
// `openrouter/<id>` means the same thing to all of them.
ProviderCatalog SessionProviderCatalog();
const Endpoint* FindEndpoint(const std::vector<Endpoint>& providers,
                             const std::string& name);
// The endpoint used when none is addressed: the first well-known one whose
// key is set, else nothing.
const Endpoint* DefaultEndpoint(const std::vector<Endpoint>& providers);
// What an endpoint declares of itself, by its address: the provider entry
// that has it, else the well-known endpoint. False when neither does.
bool DeclareEndpoint(Endpoint& endpoint,
                     const std::vector<Endpoint>& providers);
std::optional<Route> ResolveModelRoute(const std::vector<Route>& routes,
                                       const std::vector<Endpoint>& providers,
                                       const std::string& selection);
// The selection that would reproduce a route in the shared schema,
// [endpoint/]model[:effort] — the same string the user could pass to
// --model. The endpoint scope is omitted when no provider or well-known
// endpoint serves its base URL.
std::string RouteSelection(const Route& route,
                           const std::vector<Endpoint>& providers);
// The same, by the name the route was chosen by where it has one of its own
// (an alias): what a provider says of an alias is found again by that name
// only, so a session handed this selection resolves the same route.
std::string ChosenSelection(const Api& api,
                            const std::vector<Endpoint>& providers);
std::string ProviderScope(const std::string& base_url,
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
std::optional<std::vector<ModelInfo>> ParseModels(const json& response);
// The catalogue the last successful query left for this endpoint, while it is
// a day old at most; a query refreshes it.
std::optional<std::vector<ModelInfo>> CachedModels(const Api& api);
std::optional<std::vector<ModelInfo>> QueryModels(Api& api,
                                                  bool abortable = false,
                                                  int64_t timeout_s = 15);
std::string NormalizeModelQuery(std::string query);
// Display-name aware match: exact substring first, then a fallback that
// unifies spaces, underscores and hyphens ("Muse Spark" finds
// "muse-spark"). Selection stays id-based; only matching is lenient.
bool MatchesModelQuery(const std::string& haystack, const std::string& query);
ModelSearch SearchModels(const Api& api, const std::vector<Route>& routes,
                         const std::vector<Endpoint>& providers,
                         std::string query);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_PROVIDERS_PROVIDERS_H_
