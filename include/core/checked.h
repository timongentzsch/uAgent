// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CHECKED_H_
#define UAGENT_INCLUDE_CORE_CHECKED_H_
// Overflow-safe size arithmetic: checked and saturating add and multiply.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace uagent {

// The sum or product, or nothing when it does not fit in size_t.
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

// The sum, or the largest size_t when it does not fit.
inline size_t SaturatingAdd(size_t left, size_t right) {
  return CheckedAdd(left, right).value_or(std::numeric_limits<size_t>::max());
}

// Negative values count as zero: a reported count is never below nothing.
inline int64_t Nonnegative(int64_t value) { return value > 0 ? value : 0; }

inline int64_t SaturatingNonnegativeAdd(int64_t left, int64_t right) {
  left = Nonnegative(left);
  right = Nonnegative(right);
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return right > kMax - left ? kMax : left + right;
}

// Whether left + right is more than `limit`; an overflow counts as more.
inline bool AdditionExceeds(size_t left, size_t right, size_t limit) {
  std::optional<size_t> sum = CheckedAdd(left, right);
  return !sum || *sum > limit;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CHECKED_H_
