// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_ADAPTIVE_SYSTEM_H_
#define UAGENT_INCLUDE_AGENT_ADAPTIVE_SYSTEM_H_
// Process-local state shared by the agent prompt and adapt_system tool.

#include <cstddef>
#include <cstdint>
#include <string>

namespace uagent {

inline constexpr size_t kAdaptiveSystemBytes = size_t{64} * 1024;
inline constexpr size_t kAdaptiveSystemReasonBytes = 512;

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
