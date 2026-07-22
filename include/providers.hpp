#pragma once
// Provider configuration and model discovery. The REPL consumes this interface;
// it does not parse provider JSON or mutate route environment variables itself.

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "api.hpp"
#include "util.hpp"

struct ModelRoute {
    std::string name, base_url, api_key, model, effort;
    long context = 0;
};

struct ModelInfo {
    std::string id, default_effort;
    std::vector<std::string> efforts;
    long context = 0;
};

inline constexpr const char* REASONING_EFFORTS[] = {
    "none", "minimal", "low", "medium", "high", "xhigh", "max"};

inline bool valid_effort(const std::string& effort) {
    return effort.empty() ||
           std::find(std::begin(REASONING_EFFORTS), std::end(REASONING_EFFORTS), effort) !=
               std::end(REASONING_EFFORTS);
}

inline std::vector<ModelRoute> load_model_routes() {
    json providers = json::parse(env_str("UAGENT_PROVIDERS"), nullptr, false);
    std::vector<ModelRoute> routes;
    if (!providers.is_object()) return routes;
    for (const auto& [provider_name, provider] : providers.items()) try {
        if (!provider.is_object() || provider_name.find('/') != std::string::npos) continue;
        std::string base_url = provider.value("base_url", "");
        if (base_url.empty() || !provider.contains("models") ||
            !provider["models"].is_object())
            continue;
        while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
        std::string api_key = provider.value("api_key", "sk-noop");
        long context = provider.value("context", 0L);
        for (const auto& [alias, spec] : provider["models"].items()) {
            if (alias.empty() || alias.find('/') != std::string::npos) continue;
            ModelRoute route;
            route.name = provider_name + "/" + alias;
            route.base_url = base_url;
            route.api_key = api_key;
            if (spec.is_string()) route.model = spec.get<std::string>();
            else if (spec.is_object()) {
                route.model = spec.value("id", "");
                route.effort = spec.value("effort", "");
                route.context = spec.value("context", context);
            }
            if (!route.context) route.context = context;
            if (!route.model.empty() && valid_effort(route.effort))
                routes.push_back(std::move(route));
        }
    } catch (const json::exception&) { continue; }
    return routes;
}

inline const ModelRoute* find_model_route(const std::vector<ModelRoute>& routes,
                                          const std::string& name) {
    for (const ModelRoute& route : routes)
        if (route.name == name) return &route;
    return nullptr;
}

inline void export_route(const Api& api) {
    setenv("UAGENT_BASE_URL", api.base_url.c_str(), 1);
    setenv("UAGENT_API_KEY", api.api_key.c_str(), 1);
    setenv("UAGENT_MODEL", api.model.c_str(), 1);
    setenv("UAGENT_REASONING_EFFORT", api.reasoning_effort.c_str(), 1);
    setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
}

inline void apply_route(Api& api, const ModelRoute& route) {
    api.base_url = route.base_url;
    api.api_key = route.api_key.empty() ? "sk-noop" : route.api_key;
    api.model = route.model;
    api.reasoning_effort = route.effort;
    api.ctx_window = route.context;
    api.native_tools = api.include_usage = api.parallel_tools = true;
    export_route(api);
}

struct ProviderSetup {
    std::vector<ModelRoute> routes;
    std::string warning;
};

inline ProviderSetup configure_provider(Api& api) {
    api.base_url = env_str("UAGENT_BASE_URL");
    api.api_key = env_str("UAGENT_API_KEY", "sk-noop");
    api.model = env_str("UAGENT_MODEL");
    api.reasoning_effort = env_str("UAGENT_REASONING_EFFORT");
    api.ctx_window = env_long("UAGENT_CONTEXT", 0);

    ProviderSetup setup{load_model_routes(), {}};
    if (const ModelRoute* route = find_model_route(setup.routes, api.model))
        apply_route(api, *route);
    if (api.base_url.empty()) {
        std::string key = env_str("OPENROUTER_API_KEY");
        if (!key.empty()) {
            api.base_url = "https://openrouter.ai/api/v1";
            api.api_key = std::move(key);
            if (!getenv("UAGENT_MODEL"))
                api.model = env_str("OPENROUTER_MODEL", "openrouter/auto");
            if (!getenv("UAGENT_REASONING_EFFORT"))
                api.reasoning_effort = env_str("OPENROUTER_EFFORT");
            export_route(api);  // children inherit the resolved route
        }
    }
    while (!api.base_url.empty() && api.base_url.back() == '/') api.base_url.pop_back();
    if (!valid_effort(api.reasoning_effort)) {
        setup.warning = "ignoring invalid reasoning effort: " + api.reasoning_effort;
        api.reasoning_effort.clear();
        setenv("UAGENT_REASONING_EFFORT", "", 1);
    }
    return setup;
}

inline std::string select_model(Api& api, const std::vector<ModelRoute>& routes,
                                const std::string& name) {
    if (const ModelRoute* route = find_model_route(routes, name)) {
        apply_route(api, *route);
        return route->name;
    }
    if (!openrouter_url(api.base_url) || name.find('/') == std::string::npos) return "";
    if (api.model != name) {
        api.model = name;
        api.ctx_window = 0;
    }
    export_route(api);
    return api.model;
}

inline std::optional<std::vector<ModelInfo>> parse_models(const json& response,
                                                          std::string filter) {
    if (filter == "all") filter.clear();
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (!response.is_object() || !response.contains("data") ||
        !response["data"].is_array())
        return std::nullopt;

    std::vector<ModelInfo> models;
    for (const json& model : response["data"]) try {
        std::string id = model.value("id", ""), lower = id;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(tolower(c)); });
        if (id.empty() || (!filter.empty() && lower.find(filter) == std::string::npos))
            continue;
        ModelInfo info{std::move(id), {}, {}, model.value("context_length", 0L)};
        if (model.contains("reasoning") && model["reasoning"].is_object()) {
            const json& reasoning = model["reasoning"];
            info.default_effort = reasoning.value("default_effort", "");
            if (reasoning.contains("supported_efforts") &&
                reasoning["supported_efforts"].is_array())
                for (const json& effort : reasoning["supported_efforts"])
                    if (effort.is_string()) info.efforts.push_back(effort.get<std::string>());
        }
        models.push_back(std::move(info));
    } catch (const json::exception&) { continue; }
    std::sort(models.begin(), models.end(),
              [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
    return models;
}

inline std::optional<std::vector<ModelInfo>> query_models(Api& api, std::string filter) {
    return parse_models(api.get("/models"), std::move(filter));
}
