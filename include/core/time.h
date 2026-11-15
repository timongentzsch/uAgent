// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TIME_H_
#define UAGENT_INCLUDE_CORE_TIME_H_
// Clock helpers: overflow-safe deadlines, poll timeouts and wall-clock time.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace uagent {

// `seconds` after `start` on the steady clock; the clock's maximum when that
// would overflow, and `start` itself for zero or less.
inline std::chrono::steady_clock::time_point DeadlineAfter(
    std::chrono::steady_clock::time_point start, int64_t seconds) {
  if (seconds <= 0) return start;
  auto maximum = std::chrono::steady_clock::time_point::max();
  int64_t room =
      std::chrono::duration_cast<std::chrono::seconds>(maximum - start).count();
  return seconds > room ? maximum : start + std::chrono::seconds(seconds);
}

inline std::chrono::steady_clock::time_point DeadlineAfter(int64_t seconds) {
  return DeadlineAfter(std::chrono::steady_clock::now(), seconds);
}

// Whole seconds until `deadline`, rounded up so a sub-second remainder counts.
inline int64_t SecondsUntil(std::chrono::steady_clock::time_point deadline) {
  auto now = std::chrono::steady_clock::now();
  if (deadline <= now) return 0;
  return std::chrono::ceil<std::chrono::seconds>(deadline - now).count();
}

// poll(2) timeout for `deadline`, rounded up so it never fires early.
inline int PollTimeoutMs(std::chrono::steady_clock::time_point deadline) {
  auto now = std::chrono::steady_clock::now();
  if (deadline <= now) return 0;
  int64_t remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<int>(
      std::min<int64_t>(remaining, std::numeric_limits<int>::max()));
}

// Wall-clock time, for persisted timestamps.
inline int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_TIME_H_
