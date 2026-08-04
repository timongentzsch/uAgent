// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TIME_H_
#define UAGENT_INCLUDE_CORE_TIME_H_

#include <chrono>
#include <cstdint>

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

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_TIME_H_
