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

// Positive whole-second timeout for a deadline. Round up so a sub-second
// remainder remains usable by APIs whose timeout unit is seconds.
inline int64_t SecondsUntil(std::chrono::steady_clock::time_point deadline) {
  auto now = std::chrono::steady_clock::now();
  if (deadline <= now) return 0;
  return std::chrono::ceil<std::chrono::seconds>(deadline - now).count();
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

// Wall-clock time for persisted timestamps. Prefer these over hand-rolled
// duration_cast chains so every clock read looks the same.
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

// Millisecond/second conversions shared by turn metrics, retry delays and
// API progress reporting. Round up so a sub-second remainder stays usable
// by second-granularity APIs instead of collapsing to zero.
inline int64_t MillisToSecondsCeil(int64_t millis) {
  if (millis <= 0) return 0;
  return (millis + 999) / 1000;
}

inline int64_t SecondsToMillis(int64_t seconds) { return seconds * 1000; }

inline double MillisToSecondsDouble(int64_t millis) {
  return static_cast<double>(millis) / 1000.0;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_TIME_H_
