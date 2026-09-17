// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_ADAPTIVE_SYSTEM_H_
#define UAGENT_INCLUDE_AGENT_ADAPTIVE_SYSTEM_H_
// Process-local state shared by the agent prompt and adapt_system tool.

#include <cstddef>
#include <cstdint>
#include <string>

#include "include/core/limits.h"

namespace uagent {

struct AdaptiveSystemState {
  std::string instructions;
  uint64_t revision = 0;
  std::string mode = "overlay";

  void Reset() {
    instructions.clear();
    revision = 0;
    mode = "overlay";
  }
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_ADAPTIVE_SYSTEM_H_
