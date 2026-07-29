// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CHECKED_H_
#define UAGENT_INCLUDE_CORE_CHECKED_H_

#include <cstddef>
#include <limits>
#include <optional>

namespace uagent {

inline std::optional<size_t> CheckedAdd(size_t left, size_t right) {
  if (right > std::numeric_limits<size_t>::max() - left) return std::nullopt;
  return left + right;
}

inline std::optional<size_t> CheckedMul(size_t left, size_t right) {
  if (left > 0 && right > std::numeric_limits<size_t>::max() / left) {
    return std::nullopt;
  }
  return left * right;
}

inline size_t SaturatingAdd(size_t left, size_t right) {
  return CheckedAdd(left, right).value_or(std::numeric_limits<size_t>::max());
}

inline bool AdditionExceeds(size_t left, size_t right, size_t limit) {
  std::optional<size_t> sum = CheckedAdd(left, right);
  return !sum || *sum > limit;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CHECKED_H_
