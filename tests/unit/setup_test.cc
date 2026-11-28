// Copyright 2026 Timon Gentzsch

#include "include/app/setup.h"

#include <string>
#include <vector>

#include "include/api/provider_data.h"
#include "include/app/openrouter_signin.h"
#include "include/core/json.h"
#include "include/providers/providers.h"
#include "tests/unit/test_support.h"

namespace uagent {

// presets.json holds what was written in code before: the same presets, the
// same endpoints with the same features and options, and the same names.
TEST_CASE(TestProviderDataKeepsTheTableItReplaced) {
  const json& data = ProviderData();
  struct Old {
    const char* name;
    const char* label;
    const char* variable;
    const char* base_url;
    const char* wire_api;
    bool free_only;
  };
  constexpr Old kOld[] = {
      {"anthropic", "Anthropic", "ANTHROPIC_API_KEY",
       "https://api.anthropic.com/v1", "anthropic_messages", false},
      {"openai", "OpenAI", "OPENAI_API_KEY", "https://api.openai.com/v1",
       "responses", false},
      {"openrouter", "OpenRouter", "OPENROUTER_API_KEY",
       "https://openrouter.ai/api/v1", "chat_completions", false},
      {"openrouter-free", "OpenRouter Free", "OPENROUTER_FREE_API_KEY",
       "https://openrouter.ai/api/v1", "chat_completions", true},
  };
  CHECK(data["presets"].size() == std::size(kOld));
  for (size_t i = 0; i < std::size(kOld) && i < data["presets"].size(); ++i) {
    const json& preset = data["presets"][i];
    CHECK(preset["name"] == kOld[i].name);
    CHECK(preset["label"] == kOld[i].label);
    CHECK(preset["variable"] == kOld[i].variable);
    CHECK(preset["base_url"] == kOld[i].base_url);
    CHECK(preset["wire_api"] == kOld[i].wire_api);
    CHECK(JsonValue(preset, "free_only", false) == kOld[i].free_only);
    // OpenRouter's key endpoint refuses a wrong key; Anthropic lists 1000.
    const bool openrouter =
        std::string(kOld[i].base_url).find("openrouter") != std::string::npos;
    CHECK(JsonValue(preset, "key_check_path", "") ==
          (openrouter ? "/key" : ""));
    CHECK(JsonValue(preset, "models_path", "/models") ==
          (std::string(kOld[i].name) == "anthropic" ? "/models?limit=1000"
                                                    : "/models"));
  }

  Endpoint router;
  router.base_url = "https://openrouter.ai/api/v1";
  CHECK(DeclareHost(router));
  CHECK(FeatureNames(router.features) ==
        json::parse(R"(["hosted_search","provider_routing",
          "session_passthrough","raw_vendor_ids","model_variants",
          "reasoning_object","reasoning_replay_text","prompt_cache_control",
          "pdf_parser","inline_usage","decisions"])"));
  CHECK(router.options == json::parse(R"({
    "pdf_engine": "cloudflare-ai",
    "decisions_url": "https://openrouter.ai/api/alpha/decisions",
    "decisions_model": "~typesafe/jev-latest",
    "decision_bars": {
      "~typesafe/jev-latest": {"allow": 0.85, "deny": 0.95, "veto": 0.9},
      "openai/gpt-6-luna-decisions": {"allow": 0.85, "deny": 0.9, "veto": 0.9}
    }})"));
  Endpoint openai;
  openai.base_url = "https://api.openai.com/v1";
  CHECK(DeclareHost(openai));
  CHECK(FeatureNames(openai.features) ==
        json::parse(R"(["max_completion_tokens","reasoning_summary"])"));
  CHECK(openai.options.is_null());
  Endpoint local;
  local.base_url = "http://localhost:8080/v1";
  CHECK(!DeclareHost(local));

  CHECK(data["hosted_search_tools"] == json::parse(R"({
    "responses": {"type": "web_search"},
    "anthropic_messages": {"type": "web_search_20250305", "name": "web_search"},
    "chat_completions": {"type": "openrouter:web_search"}})"));
  CHECK(data["anthropic_version"] == "2023-06-01");
}

TEST_CASE(TestSetupSavesKeys) {
  SettingValues scope;
  ApplySetupKeys({{"openrouter", "sk-or"}}, scope);
  CHECK(scope.at("OPENROUTER_API_KEY") == "sk-or");
  json providers = json::parse(scope.at("UAGENT_PROVIDERS"));
  CHECK(providers["openrouter"]["api_key"] == "$OPENROUTER_API_KEY");
  CHECK(providers["openrouter"]["base_url"] == "https://openrouter.ai/api/v1");
  CHECK(providers["openrouter"]["wire_api"] == "chat_completions");

  scope.clear();
  ApplySetupKeys({{"openai", "sk-oa"}, {"anthropic", "sk-ant"}}, scope);
  CHECK(scope.at("OPENAI_API_KEY") == "sk-oa");
  CHECK(scope.at("ANTHROPIC_API_KEY") == "sk-ant");
  providers = json::parse(scope.at("UAGENT_PROVIDERS"));
  CHECK(providers["anthropic"]["api_key"] == "$ANTHROPIC_API_KEY");
  CHECK(providers["anthropic"]["wire_api"] == "anthropic_messages");
  CHECK(providers["anthropic"]["base_url"] == "https://api.anthropic.com/v1");
  CHECK(providers["openai"]["api_key"] == "$OPENAI_API_KEY");
  CHECK(providers["openai"]["wire_api"] == "responses");

  scope = {{"UAGENT_PROVIDERS",
            R"({"local":{"base_url":"http://localhost:8080/v1"},)"
            R"("anthropic":{"base_url":"https://proxy.example/v1",)"
            R"("models":{"s":"claude-sonnet-5-5"}}})"}};
  ApplySetupKeys({{"anthropic", "sk-ant"}}, scope);
  providers = json::parse(scope.at("UAGENT_PROVIDERS"));
  CHECK(providers["local"]["base_url"] == "http://localhost:8080/v1");
  CHECK(providers["anthropic"]["base_url"] == "https://proxy.example/v1");
  CHECK(providers["anthropic"]["models"]["s"] == "claude-sonnet-5-5");
  CHECK(providers["anthropic"]["api_key"] == "$ANTHROPIC_API_KEY");
  CHECK(!providers.contains("openai"));

  scope = {{"UAGENT_PROVIDERS", "$MY_PROVIDERS"}};
  ApplySetupKeys({{"openai", "sk-oa"}}, scope);
  CHECK(scope.at("UAGENT_PROVIDERS") == "$MY_PROVIDERS");

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
  ApplySetupKeys({{"openrouter-free", "free-key"}}, scope);
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

  // The free ones are listed by how much they write, then read, then age;
  // any other catalogue keeps its order.
  const auto named = [&](const char* id, int64_t writes, int64_t reads,
                         int64_t created) {
    json model = eligible;
    model.update({{"id", id},
                  {"context_length", reads},
                  {"created", created},
                  {"top_provider", {{"max_completion_tokens", writes}}}});
    return model;
  };
  const json catalogue = {{"data",
                           {named("small:free", 8192, 1048576, 9),
                            named("old:free", 131072, 262144, 1),
                            named("new:free", 131072, 262144, 2),
                            named("wide:free", 131072, 1000000, 0),
                            named("long:free", 921600, 1024000, 5)}}};
  const auto ids = [&](bool free_only) {
    const auto models = ParseModels(catalogue, free_only);
    std::string out;
    for (const ModelInfo& info : *models) out += info.id + " ";
    return out;
  };
  CHECK(ids(true) == "long:free wide:free new:free old:free small:free ");
  CHECK(ids(false).starts_with("small:free "));
}

// RFC 7636, appendix B.
TEST_CASE(TestSignInChallenge) {
  CHECK(PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
        "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  const SignIn pasted = BeginSignIn("https://openrouter.ai/api/v1", "");
  CHECK(pasted.url.starts_with("https://openrouter.ai/auth?code_challenge=" +
                               PkceChallenge(pasted.verifier)));
  CHECK(pasted.url.find("callback_url") == std::string::npos);
  const SignIn sent = BeginSignIn("https://openrouter.ai/api/v1",
                                  "http://localhost:9/callback");
  CHECK(sent.url.ends_with(
      "&state=" + sent.state +
      "&callback_url=http%3A%2F%2Flocalhost%3A9%2Fcallback"));
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
