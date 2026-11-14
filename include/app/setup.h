// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SETUP_H_
#define UAGENT_INCLUDE_APP_SETUP_H_
// `uagent setup`: saves provider keys and a default model for all
// conversations, so a first start needs no environment.

#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/core/settings_store.h"

namespace uagent {

// The keys to save; an empty one leaves what is saved as it is.
struct SetupKeys {
  std::string openrouter = "", openai = "", anthropic = "",
              openrouter_free = "";
};

// Writes each key into a variable that a provider of that name refers to.
// Other providers, and fields a saved provider already has, stay.
void ApplySetupKeys(const SetupKeys& keys, SettingValues& scope);

// The model offered first for `provider` (openrouter, openai or anthropic)
// from what its endpoint lists: a Sonnet, the newest GPT, or the first id.
std::string SuggestModel(const std::string& provider,
                         const std::vector<std::string>& ids);

// Whether a provider resolves from what is saved and set, asked of a child
// process so this one's settings stay as they were.
bool ProviderConfigured();

// The interactive flow; 0 when something was saved or kept. Needs a terminal.
int RunSetup();

// Signing in to OpenRouter for the web: `start` returns the address to
// approve at (with `callback`, OpenRouter sends the browser back there), and
// `finish` redeems the code, saves the key as the free provider and returns
// the default model.
json OpenRouterControl(const json& request);

// One line from stdin with echo off where it is a terminal, as typed.
std::string ReadHidden();
// `uagent setup`, or `uagent setup openrouter [--no-browser]` to sign in.
int SetupMain(int argc, char** argv);

}  // namespace uagent
#endif
