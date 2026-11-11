// Copyright 2026 Timon Gentzsch

#include "include/app/setup.h"

#include <string>
#include <vector>

#include "include/core/json.h"
#include "tests/unit/test_support.h"

namespace uagent {

TEST_CASE(TestSetupSavesKeys) {
  // Each key becomes a provider that refers to its variable; OpenRouter is
  // one like the others.
  SettingValues scope;
  ApplySetupKeys({.openrouter = "sk-or"}, scope);
  CHECK(scope.at("OPENROUTER_API_KEY") == "sk-or");
  json providers = json::parse(scope.at("UAGENT_PROVIDERS"));
  CHECK(providers["openrouter"]["api_key"] == "$OPENROUTER_API_KEY");
  CHECK(providers["openrouter"]["base_url"] == "https://openrouter.ai/api/v1");
  CHECK(providers["openrouter"]["wire_api"] == "chat_completions");

  scope.clear();
  ApplySetupKeys({.openai = "sk-oa", .anthropic = "sk-ant"}, scope);
  CHECK(scope.at("OPENAI_API_KEY") == "sk-oa");
  CHECK(scope.at("ANTHROPIC_API_KEY") == "sk-ant");
  providers = json::parse(scope.at("UAGENT_PROVIDERS"));
  CHECK(providers["anthropic"]["api_key"] == "$ANTHROPIC_API_KEY");
  CHECK(providers["anthropic"]["wire_api"] == "anthropic_messages");
  CHECK(providers["anthropic"]["base_url"] == "https://api.anthropic.com/v1");
  CHECK(providers["openai"]["api_key"] == "$OPENAI_API_KEY");
  CHECK(providers["openai"]["wire_api"] == "responses");

  // Other providers and what a saved one of the same name says stay.
  scope = {{"UAGENT_PROVIDERS",
            R"({"local":{"base_url":"http://localhost:8080/v1"},)"
            R"("anthropic":{"base_url":"https://proxy.example/v1",)"
            R"("models":{"s":"claude-sonnet-5-5"}}})"}};
  ApplySetupKeys({.anthropic = "sk-ant"}, scope);
  providers = json::parse(scope.at("UAGENT_PROVIDERS"));
  CHECK(providers["local"]["base_url"] == "http://localhost:8080/v1");
  CHECK(providers["anthropic"]["base_url"] == "https://proxy.example/v1");
  CHECK(providers["anthropic"]["models"]["s"] == "claude-sonnet-5-5");
  CHECK(providers["anthropic"]["api_key"] == "$ANTHROPIC_API_KEY");
  CHECK(!providers.contains("openai"));

  // Providers set from a variable are left to that variable.
  scope = {{"UAGENT_PROVIDERS", "$MY_PROVIDERS"}};
  ApplySetupKeys({.openai = "sk-oa"}, scope);
  CHECK(scope.at("UAGENT_PROVIDERS") == "$MY_PROVIDERS");

  // No key, no change.
  scope.clear();
  ApplySetupKeys({}, scope);
  CHECK(scope.empty());
}

TEST_CASE(TestSetupFreeProvider) {
  SettingValues scope{
      {"OPENROUTER_API_KEY", "paid-key"},
      {"UAGENT_MODEL", "openrouter/paid-model"},
      {"UAGENT_PROVIDERS",
       R"({"openrouter":{"base_url":"https://openrouter.ai/api/v1","api_key":"$OPENROUTER_API_KEY"}})"}};
  const json paid = json::parse(scope.at("UAGENT_PROVIDERS"))["openrouter"];
  ApplySetupKeys({.openrouter_free = "free-key"}, scope);
  const json providers = json::parse(scope.at("UAGENT_PROVIDERS"));
  CHECK(providers["openrouter"] == paid);
  CHECK(scope.at("OPENROUTER_API_KEY") == "paid-key");
  CHECK(scope.at("UAGENT_MODEL") == "openrouter/paid-model");
  CHECK(scope.at("OPENROUTER_FREE_API_KEY") == "free-key");
  CHECK(providers["openrouter-free"]["api_key"] == "$OPENROUTER_FREE_API_KEY");
  CHECK(providers["openrouter-free"]["options"]["free_only"] == true);

  const ScopedEnv configured("UAGENT_PROVIDERS",
                             scope.at("UAGENT_PROVIDERS").c_str());
  const ProviderCatalog catalog = LoadProviderCatalog();
  const Endpoint* free = FindEndpoint(catalog.providers, "openrouter-free");
  CHECK(free && free->FreeOnly());
  CHECK(!free->features.Has(Feature::kHostedSearch));
  CHECK(!free->features.Has(Feature::kPdfParser));
  CHECK(!free->features.Has(Feature::kDecisions));
  CHECK(free->hosted_tools.empty());
  Api api(RuntimeConfig{});
  static_cast<Endpoint&>(api) = *free;
  api.model = "vendor/coder:free";
  api.options["provider"] = "Example";
  const json body = api.BuildRequestBody(json::array(), json::array());
  CHECK(body["provider"]["max_price"] ==
        json({{"prompt", 0}, {"completion", 0}, {"request", 0}, {"image", 0}}));
  CHECK(body["provider"]["order"] == json::array({"Example"}));
  CHECK(body["provider"]["allow_fallbacks"] == false);
  CHECK(!body.contains("plugins"));
  CHECK(RouteSelection(api, catalog.providers) ==
        "openrouter-free/vendor/coder:free");
  api.name.clear();  // Child routes inherit the price policy without a name.
  CHECK(RouteSelection(api, catalog.providers) ==
        "openrouter-free/vendor/coder:free");
  const Endpoint* normal = FindEndpoint(catalog.providers, "openrouter");
  CHECK(normal && !normal->FreeOnly());
  CHECK(normal->features.Has(Feature::kPdfParser));
  CHECK(normal->features.Has(Feature::kDecisions));
  static_cast<Endpoint&>(api) = *normal;
  CHECK(
      !api.BuildRequestBody(json::array(), json::array()).contains("provider"));
}

TEST_CASE(TestSetupFreeCatalogue) {
  const json eligible = {
      {"id", "vendor/coder:free"},
      {"context_length", 65536},
      {"supported_parameters", {"tools", "temperature"}},
      {"pricing", {{"prompt", "0"}, {"completion", 0}, {"request", "0.0"}}}};
  const auto parse = [](const json& model, bool free_only = true) {
    return ParseModels({{"data", json::array({model})}}, free_only);
  };
  CHECK(parse(eligible)->size() == 1);
  for (const json& change : std::vector<json>{
           {{"id", "vendor/coder"}},
           {{"context_length", 65535}},
           {{"supported_parameters", {"temperature"}}},
           {{"pricing", {{"prompt", 0}}}},
           {{"pricing", {{"prompt", 0}, {"completion", "0.001"}}}},
           {{"pricing", {{"prompt", 0}, {"completion", 0}, {"image", "0.01"}}}},
           {{"pricing",
             {{"prompt", 0}, {"completion", 0}, {"request", "unknown"}}}},
           {{"pricing", {{"prompt", -1}, {"completion", 0}}}},
           {{"pricing", {{"prompt", nullptr}, {"completion", 0}}}}}) {
    json rejected = eligible;
    rejected.update(change);
    CHECK(parse(rejected)->empty());
    CHECK(parse(rejected, false)->size() == 1);
  }
  json missing = eligible;
  missing.erase("pricing");
  CHECK(parse(missing)->empty());
  missing = eligible;
  missing.erase("supported_parameters");
  CHECK(parse(missing)->empty());
}

TEST_CASE(TestSetupSuggestsModel) {
  CHECK(SuggestModel("anthropic", {"claude-haiku-5-5", "claude-sonnet-4-5",
                                   "claude-sonnet-5-5", "claude-opus-5-5"}) ==
        "claude-sonnet-5-5");
  CHECK(
      SuggestModel("openrouter", {"openai/gpt-6", "anthropic/claude-sonnet-5.5",
                                  "anthropic/claude-sonnet-5.5:free",
                                  "anthropic/claude-sonnet-4.5"}) ==
      "anthropic/claude-sonnet-5.5");
  CHECK(SuggestModel("openai",
                     {"gpt-5.5", "gpt-6", "gpt-6-mini", "gpt-6-2026-01-01",
                      "gpt-6-luna-decisions", "text-embedding-3"}) == "gpt-6");
  // Nothing preferred: the first listed.
  CHECK(SuggestModel("openai", {"o9", "o8"}) == "o9");
  CHECK(SuggestModel("anthropic", {}).empty());
}

}  // namespace uagent
