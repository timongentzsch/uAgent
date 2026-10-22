// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
namespace {
// The well-known endpoints, as data: an address, the variable its key is read
// from (empty: none is offered by itself) and what it declares. An endpoint
// is available when its key is set; a provider entry of the same name only
// states what differs. This table and the provider entries are the only
// places a vendor is named.
struct KnownEndpoint {
  const char* name;
  const char* base_url;
  const char* host;
  const char* key_variable;
  FeatureSet features;
  const char* options;
};

// The decision models the endpoint serves: the address, the model used when
// none is chosen, and for each model the probabilities that decide a review
// (see tests/reviewer_eval, which finds them): allow needs P(allow) at least
// `allow`, deny needs P(deny) at least `deny`, and any reason question at or
// above `veto` asks instead of allowing. A model with no row here gets the
// strict bars of permissions.cc.
constexpr char kOpenRouterOptions[] = R"({
  "pdf_engine": "cloudflare-ai",
  "decisions_url": "https://openrouter.ai/api/alpha/decisions",
  "decisions_model": "~typesafe/jev-latest",
  "decision_bars": {
    "~typesafe/jev-latest": {"allow": 0.85, "deny": 0.95, "veto": 0.9},
    "openai/gpt-6-luna-decisions": {"allow": 0.85, "deny": 0.9, "veto": 0.9}
  }
})";

constexpr KnownEndpoint kKnownEndpoints[] = {
    {"openrouter", "https://openrouter.ai/api/v1", "openrouter.ai",
     "OPENROUTER_API_KEY",
     FeaturesOf({Feature::kSearchPlugin, Feature::kProviderRouting,
                 Feature::kSessionPassthrough, Feature::kRawVendorIds,
                 Feature::kModelVariants, Feature::kReasoningObject,
                 Feature::kReasoningReplayText, Feature::kPromptCacheControl,
                 Feature::kPdfParser, Feature::kInlineUsage,
                 Feature::kDecisions}),
     kOpenRouterOptions},
    {"openai", "https://api.openai.com/v1", "api.openai.com", "",
     FeaturesOf({Feature::kMaxCompletionTokens, Feature::kSearchSources,
                 Feature::kReasoningSummary}),
     ""},
};

const KnownEndpoint* KnownEndpointForUrl(const std::string& url) {
  const std::string host = UrlHost(url);
  for (const KnownEndpoint& known : kKnownEndpoints) {
    const std::string name = known.host;
    if (host == name || host.ends_with("." + name)) return &known;
  }
  return nullptr;
}

void Declare(Endpoint& endpoint, const KnownEndpoint& known) {
  endpoint.features = known.features;
  endpoint.options = *known.options ? json::parse(known.options) : json();
}

constexpr std::chrono::hours kCatalogueLife{24};
constexpr size_t kCatalogueCacheBytes = MiB(8);

// What a catalogue answers for: the endpoint and the way it is asked. The
// credential is no part of it, so nothing derived from one is written down.
std::string CatalogueIdentity(const Api& api) {
  return api.base_url + "\n" + WireApiName(api.wire_api);
}

std::string CataloguePath(const std::string& identity) {
  return UagentDir(kModelsDir) + "/" + HashHex(identity) + ".json";
}

}  // namespace
std::string NormalizeModelId(std::string model) {
  model = Trim(model);
  bool separator = false;
  std::string normalized;
  normalized.reserve(model.size());
  for (char raw : model) {
    const unsigned char value = Byte(raw);
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

const Endpoint* FindEndpoint(const std::vector<Endpoint>& providers,
                             const std::string& name) {
  for (const Endpoint& provider : providers) {
    if (provider.name == name) return &provider;
  }
  return nullptr;
}

// The provider serving a base URL, else the well-known endpoint whose name
// selects it; a custom endpoint has no name to scope with.
std::string ProviderScope(const std::string& base_url,
                          const std::vector<Endpoint>& providers) {
  for (const Endpoint& provider : providers) {
    if (provider.base_url == base_url) return provider.name;
  }
  const KnownEndpoint* known = KnownEndpointForUrl(base_url);
  return known && *known->key_variable ? known->name : std::string();
}

const Endpoint* DefaultEndpoint(const std::vector<Endpoint>& providers) {
  for (const KnownEndpoint& known : kKnownEndpoints) {
    if (!*known.key_variable) continue;
    if (const Endpoint* endpoint = FindEndpoint(providers, known.name)) {
      return endpoint;
    }
  }
  return nullptr;
}

bool DeclareEndpoint(Endpoint& endpoint,
                     const std::vector<Endpoint>& providers) {
  for (const Endpoint& provider : providers) {
    if (provider.base_url != endpoint.base_url) continue;
    endpoint.name = provider.name;
    endpoint.features = provider.features;
    endpoint.options = provider.options;
    endpoint.hosted_tools = provider.hosted_tools;
    return true;
  }
  const KnownEndpoint* known = KnownEndpointForUrl(endpoint.base_url);
  if (known) Declare(endpoint, *known);
  return known != nullptr;
}

ProviderCatalog LoadProviderCatalog() {
  json providers =
      json::parse(SettingText(Cfg("UAGENT_PROVIDERS")), nullptr, false);
  ProviderCatalog catalog;
  if (!providers.is_object()) return catalog;
  for (const auto& [provider_name, provider] : providers.items()) {
    if (!provider.is_object() || provider_name.empty() ||
        provider_name == "all" ||
        provider_name.find('/') != std::string::npos) {
      continue;
    }
    // An entry named like a well-known endpoint may state only what differs.
    const KnownEndpoint* known = nullptr;
    for (const KnownEndpoint& row : kKnownEndpoints) {
      if (provider_name == row.name) known = &row;
    }
    Endpoint endpoint;
    endpoint.name = provider_name;
    std::string base_url = JsonValue(provider, "base_url", "");
    endpoint.api_key = JsonValue(provider, "api_key", kPlaceholderApiKey);
    if (base_url.empty() && known) {
      base_url = known->base_url;
      if (!provider.contains("api_key")) {
        endpoint.api_key =
            *known->key_variable ? SettingText(known->key_variable) : "";
        if (endpoint.api_key.empty()) continue;
      }
    }
    if (base_url.empty()) continue;
    endpoint.base_url = StripTrailingSlashes(std::move(base_url));
    endpoint.context = JsonValue(provider, "context", int64_t{0});
    std::optional<WireApi> wire_api =
        ParseWireApi(JsonValue(provider, "wire_api", "chat_completions"));
    if (!wire_api) continue;
    endpoint.wire_api = *wire_api;
    if (const KnownEndpoint* row = KnownEndpointForUrl(endpoint.base_url)) {
      Declare(endpoint, *row);
    }
    if (provider.contains("supports")) {
      std::string unknown;
      endpoint.features = {};
      if (!ParseFeatures(provider["supports"], endpoint.features, unknown)) {
        continue;
      }
    }
    const json hosted = JsonValue(provider, "hosted_tools", json::array());
    if (HasHostedTool(hosted, HostedTool::kWebSearch)) {
      endpoint.features.Add(Feature::kHostedSearch);
    }
    endpoint.hosted_tools = HostedToolDefinitions(hosted);
    if (const json* options = JsonObject(provider, "options")) {
      if (!endpoint.options.is_object()) endpoint.options = json::object();
      endpoint.options.update(*options);
    }
    endpoint.model_features = JsonValue(provider, "features", json(nullptr));
    catalog.providers.push_back(endpoint);
    if (!provider.contains("models") || !provider["models"].is_object()) {
      continue;
    }
    for (const auto& [alias, spec] : provider["models"].items()) {
      if (alias.empty() || alias.find('/') != std::string::npos) continue;
      Route route;
      static_cast<Endpoint&>(route) = endpoint;
      route.route_name = provider_name + "/" + alias;
      if (spec.is_string()) {
        route.model = spec.get<std::string>();
      } else if (spec.is_object()) {
        route.model = JsonValue(spec, "id", "");
        if (const json* features = JsonObject(spec, "features")) {
          if (!route.model_features.is_object()) {
            route.model_features = json::object();
          }
          route.model_features.update(*features);
        }
        route.reasoning_effort = JsonValue(spec, "effort", "");
        route.ctx_window = JsonValue(spec, "context", endpoint.context);
        if (spec.contains("wire_api")) {
          std::optional<WireApi> model_wire =
              ParseWireApi(JsonValue(spec, "wire_api", ""));
          if (!model_wire) continue;
          route.wire_api = *model_wire;
        }
        if (spec.contains("hosted_tools")) {
          if (HasHostedTool(spec["hosted_tools"], HostedTool::kWebSearch)) {
            route.features.Add(Feature::kHostedSearch);
          } else {
            route.features.Remove(Feature::kHostedSearch);
          }
          route.hosted_tools = HostedToolDefinitions(spec["hosted_tools"]);
        }
      }
      if (!route.ctx_window) route.ctx_window = endpoint.context;
      if (!route.model.empty() && ValidEffort(route.reasoning_effort)) {
        catalog.models.push_back(std::move(route));
      }
    }
  }
  return catalog;
}

ProviderCatalog SessionProviderCatalog() {
  ProviderCatalog catalog = LoadProviderCatalog();
  for (const KnownEndpoint& known : kKnownEndpoints) {
    std::string api_key =
        *known.key_variable ? SettingText(known.key_variable) : std::string();
    if (api_key.empty() || FindEndpoint(catalog.providers, known.name)) {
      continue;
    }
    Endpoint endpoint;
    endpoint.name = known.name;
    endpoint.base_url = known.base_url;
    endpoint.api_key = std::move(api_key);
    Declare(endpoint, known);
    catalog.providers.push_back(std::move(endpoint));
  }
  return catalog;
}

int64_t CatalogContextLength(const json& model) {
  // Anthropic's catalog spells the same window max_input_tokens.
  for (const char* key :
       {"context_length", "max_input_tokens", "max_model_len"}) {
    if (int64_t context = JsonValue(model, key, int64_t{0})) return context;
  }
  const json* meta = JsonObject(model, "meta");
  return meta ? JsonValue(*meta, "n_ctx_train", int64_t{0}) : 0;
}

std::optional<std::vector<ModelInfo>> ParseModels(const json& response) {
  const json* data = JsonArray(response, "data");
  if (!data) return std::nullopt;

  std::vector<ModelInfo> models;
  for (const json& model : *data) {
    if (!model.is_object()) continue;
    std::string id = JsonValue(model, "id", "");
    if (id.empty()) continue;
    ModelInfo info{std::move(id), {}, {}, CatalogContextLength(model)};
    info.name = JsonValue(model, "name", JsonValue(model, "display_name", ""));
    info.features = JsonValue(model, "features", json(nullptr));
    if (const json* capabilities = JsonObject(model, "capabilities")) {
      if (const json* thinking = JsonObject(*capabilities, "thinking")) {
        if (const json* types = JsonObject(*thinking, "types")) {
          if (const json* adaptive = JsonObject(*types, "adaptive")) {
            if (!info.features.is_object()) info.features = json::object();
            const bool supported = JsonValue(*adaptive, "supported", false);
            info.features["adaptive_thinking"] = supported;
            info.features["reasoning_summary"] = supported;
          }
        }
      }
      if (const json* effort = JsonObject(*capabilities, "effort")) {
        for (std::string_view name : kReasoningEfforts) {
          const std::string level(name);
          if (const json* support = JsonObject(*effort, level.c_str())) {
            if (JsonValue(*support, "supported", false)) {
              info.efforts.emplace_back(level);
            }
          }
        }
      }
    }
    const json* modalities = JsonArray(model, "input_modalities");
    if (const json* architecture = JsonObject(model, "architecture")) {
      if (!modalities) {
        modalities = JsonArray(*architecture, "input_modalities");
      }
    }
    if (modalities) info.input_modalities = *modalities;
    const json* efforts = JsonArray(model, "supported_reasoning_efforts");
    if (const json* defaults = JsonObject(model, "default_parameters")) {
      info.default_effort = JsonValue(*defaults, "reasoning_effort", "");
    }
    if (const json* reasoning = JsonObject(model, "reasoning")) {
      if (!efforts) efforts = JsonArray(*reasoning, "supported_efforts");
      if (info.default_effort.empty()) {
        info.default_effort = JsonValue(*reasoning, "default_effort", "");
      }
    }
    if (efforts) {
      info.efforts.clear();
      for (const json& effort : *efforts) {
        if (effort.is_string() && ValidEffort(effort.get<std::string>())) {
          info.efforts.push_back(effort.get<std::string>());
        }
      }
    }
    models.push_back(std::move(info));
  }
  return models;
}

std::optional<std::vector<ModelInfo>> CachedModels(const Api& api) {
  const std::string identity = CatalogueIdentity(api);
  const std::string path = CataloguePath(identity);
  std::error_code ignored;
  const auto written = std::filesystem::last_write_time(path, ignored);
  std::string text, error;
  if (ignored ||
      std::filesystem::file_time_type::clock::now() - written >=
          kCatalogueLife ||
      !ReadRegularFile(path, kCatalogueCacheBytes, text, error)) {
    return std::nullopt;
  }
  const json file = json::parse(text, nullptr, false);
  const json* response = JsonObject(file, "response");
  if (JsonValue(file, "identity", "") != identity || !response) {
    return std::nullopt;
  }
  auto models = ParseModels(*response);
  if (!models || models->empty()) return std::nullopt;
  return models;
}

// A start reads the catalogue from CachedModels instead of waiting on the
// provider, and what is learned from it (context window, modalities) changes
// rarely; every answer that is usable keeps that copy current.
std::optional<std::vector<ModelInfo>> QueryModels(Api& api, bool abortable,
                                                  int64_t timeout_s) {
  JsonResponse response = api.Get("/models", abortable, timeout_s);
  if (!response.error.empty()) return std::nullopt;
  auto models = ParseModels(response.body);
  if (models && !models->empty()) {
    const std::string identity = CatalogueIdentity(api);
    std::string error;
    AtomicWriteFile(CataloguePath(identity),
                    JsonDump({{"identity", identity},
                              {"response", std::move(response.body)}}),
                    kPrivateFileMode, false, error);
  }
  return models;
}

}  // namespace uagent
