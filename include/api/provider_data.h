// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_PROVIDER_DATA_H_
#define UAGENT_INCLUDE_API_PROVIDER_DATA_H_
// What uAgent knows of particular providers, from src/providers/presets.json:
// setup's presets, the endpoints it recognizes, the hosted search tool each
// wire API names, and the Anthropic API version it speaks.

#include "include/core/json.h"

namespace uagent {

const json& ProviderData();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_PROVIDER_DATA_H_
