// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_LIMITS_H_
#define UAGENT_INCLUDE_CORE_LIMITS_H_
// Compile-time policy constants that several modules must agree on. Nothing
// here is tunable: runtime-adjustable values belong in the EnvBounded/EnvStr
// table in src/core/env.cc and in RuntimeConfig. Keep this header dependency
// free -- it is included widely.

#include <cstddef>
#include <cstdint>

namespace uagent {

// Agent-private artifacts (sessions, logs, memories, config). Owner-only, so a
// shared machine cannot read transcripts or credentials out of ~/.uagent.
inline constexpr int kPrivateFileMode = 0600;
inline constexpr int kPrivateDirMode = 0700;

// Files written into the user's workspace on their behalf, which follow the
// usual world-readable convention rather than the private-artifact policy.
inline constexpr int kSharedFileMode = 0644;

// Bounds for the `run` tool's initial yield. The lower bound keeps a yield from
// degenerating into a busy poll; the upper bound keeps one call from consuming
// the turn. The tool schema, the argument validator, the clamp in the shell
// runner and the UAGENT_RUN_YIELD_MS env bound must agree.
inline constexpr int64_t kMinYieldMs = 250;
inline constexpr int64_t kMaxYieldMs = 30000;

// Settle time for an activity write or resize that named no wait_ms: long
// enough for the child to echo, short enough not to feel like a wait.
inline constexpr int64_t kActivityInputSettleMs = 250;

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_LIMITS_H_
