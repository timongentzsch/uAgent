// Copyright 2026 Timon Gentzsch

#include "include/app/setup.h"

#include <string>
#include <vector>

#include "include/core/json.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestSetupSavesKeys() {
  // OpenRouter's key is its own setting; nothing else is touched.
  SettingValues scope;
  ApplySetupKeys({.openrouter = "sk-or"}, scope);
  CHECK(scope.at("OPENROUTER_API_KEY") == "sk-or");
  CHECK(!scope.contains("UAGENT_PROVIDERS"));

  // OpenAI and Anthropic become providers that refer to their variables.
  scope.clear();
  ApplySetupKeys({.openai = "sk-oa", .anthropic = "sk-ant"}, scope);
  CHECK(scope.at("OPENAI_API_KEY") == "sk-oa");
  CHECK(scope.at("ANTHROPIC_API_KEY") == "sk-ant");
  json providers = json::parse(scope.at("UAGENT_PROVIDERS"));
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

void TestSetupSuggestsModel() {
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
