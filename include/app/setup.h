// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SETUP_H_
#define UAGENT_INCLUDE_APP_SETUP_H_
// `uagent setup`: asks for the OpenRouter, OpenAI and Anthropic keys and a
// default model, and saves them for all conversations, so a first start
// needs no environment.

#include <string>
#include <vector>

#include "include/core/settings_store.h"

namespace uagent {

// The keys to save; an empty one leaves what is saved as it is.
struct SetupKeys {
  std::string openrouter = "", openai = "", anthropic = "";
};

// Writes `keys` into one scope: OpenRouter's into its setting, OpenAI's and
// Anthropic's into variables that a provider of that name refers to. Other
// providers, and fields a saved provider of that name already has, stay.
void ApplySetupKeys(const SetupKeys& keys, SettingValues& scope);

// The model offered first for `provider` (openrouter, openai or anthropic)
// from what its endpoint lists: a Sonnet, the newest GPT, or the first id.
std::string SuggestModel(const std::string& provider,
                         const std::vector<std::string>& ids);

// Whether a provider resolves from what is saved and set, asked of a child
// process so this one's settings stay as they were.
bool ProviderConfigured();

// The interactive flow; 0 when something was saved or kept. Needs a
// terminal.
int RunSetup();
int SetupMain();

}  // namespace uagent
#endif
