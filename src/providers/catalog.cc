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
constexpr ProviderTemplate kProviderTemplates[] = {{
    "openrouter",
    "https://openrouter.ai/api/v1",
    "OPENROUTER_API_KEY",
    OpenrouterUrl,
    ProviderProtocol::kOpenRouter,
    WireApi::kChatCompletions,
    false,
}};

constexpr std::chrono::hours kCatalogueLife{24};
constexpr size_t kCatalogueCacheBytes = size_t{8} * 1024 * 1024;

// What a catalogue answers for: the endpoint and the way it is asked. The
// credential is no part of it, so nothing derived from one is written down.
std::string CatalogueIdentity(const Api& api) {
  return api.base_url + "\n" + ProviderProtocolName(api.capabilities.protocol) +
         "\n" + WireApiName(api.capabilities.wire_api);
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

const ProviderTemplate* FindProviderTemplateForUrl(const std::string& url) {
  for (const ProviderTemplate& provider : kProviderTemplates) {
    if (provider.matches_url(url)) return &provider;
  }
  return nullptr;
}

const ProviderTemplate& DefaultProviderTemplate() {
  return kProviderTemplates[0];
}

bool ApplyProviderTemplate(Api& api, const ProviderTemplate& provider) {
  std::string api_key = SettingText(provider.api_key_env);
  if (api_key.empty()) return false;
  api.base_url = provider.base_url;
  api.api_key = std::move(api_key);
  api.capabilities =
      CapabilitiesForRoute(provider.protocol, api.base_url, provider.wire_api,
                           provider.hosted_web_search);
  return true;
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
    std::string base_url = JsonValue(provider, "base_url", "");
    if (base_url.empty()) continue;
    base_url = StripTrailingSlashes(std::move(base_url));
    std::string api_key = JsonValue(provider, "api_key", kPlaceholderApiKey);
    int64_t context = JsonValue(provider, "context", int64_t{0});
    std::optional<WireApi> wire_api =
        ParseWireApi(JsonValue(provider, "wire_api", "chat_completions"));
    if (!wire_api) continue;
    std::string protocol_name = JsonValue(provider, "protocol", "");
    std::optional<ProviderProtocol> configured_protocol;
    if (!protocol_name.empty()) {
      configured_protocol = ParseProviderProtocol(protocol_name);
      if (!configured_protocol) continue;
    }
    ProviderProtocol protocol =
        configured_protocol.value_or(ProviderProtocol::kOpenAi);
    bool hosted_web_search =
        HasHostedTool(JsonValue(provider, "hosted_tools", json::array()),
                      HostedTool::kWebSearch);
    catalog.providers.push_back(
        {provider_name, base_url, api_key, context, protocol, *wire_api,
         hosted_web_search, JsonValue(provider, "features", json(nullptr))});
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
      route.wire_api = *wire_api;
      route.hosted_web_search = hosted_web_search;
      route.features = JsonValue(provider, "features", json(nullptr));
      if (spec.is_string()) {
        route.model = spec.get<std::string>();
      } else if (spec.is_object()) {
        route.model = JsonValue(spec, "id", "");
        if (const json* features = JsonObject(spec, "features")) {
          if (!route.features.is_object()) route.features = json::object();
          route.features.update(*features);
        }
        route.effort = JsonValue(spec, "effort", "");
        route.context = JsonValue(spec, "context", context);
        if (spec.contains("wire_api")) {
          std::optional<WireApi> model_wire =
              ParseWireApi(JsonValue(spec, "wire_api", ""));
          if (!model_wire) continue;
          route.wire_api = *model_wire;
        }
        if (spec.contains("hosted_tools")) {
          route.hosted_web_search =
              HasHostedTool(spec["hosted_tools"], HostedTool::kWebSearch);
        }
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
    std::string api_key = SettingText(provider.api_key_env);
    if (api_key.empty() ||
        FindNamedProvider(catalog.providers, provider.name)) {
      continue;
    }
    catalog.providers.push_back(
        {provider.name, provider.base_url, std::move(api_key), 0,
         provider.protocol, provider.wire_api, provider.hosted_web_search});
  }
}

ProviderCatalog SessionProviderCatalog() {
  ProviderCatalog catalog = LoadProviderCatalog();
  AddAvailableProviderTemplates(catalog);
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
