// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TIME_H_
#define UAGENT_INCLUDE_CORE_TIME_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace uagent {

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

// Whole seconds left, truncated toward zero and negative once past.
inline int64_t SecondsUntil(std::chrono::steady_clock::time_point deadline) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             deadline - std::chrono::steady_clock::now())
      .count();
}

// poll(2) timeout for a steady-clock deadline, rounded up so a sub-millisecond
// remainder does not become a premature timeout.
inline int PollTimeoutMs(std::chrono::steady_clock::time_point deadline) {
  auto now = std::chrono::steady_clock::now();
  if (deadline <= now) return 0;
  int64_t remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<int>(
      std::min<int64_t>(remaining, std::numeric_limits<int>::max()));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_TIME_H_
