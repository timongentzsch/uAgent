// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_OUTPUT_BUFFER_H_
#define UAGENT_INCLUDE_CORE_OUTPUT_BUFFER_H_

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/strings.h"

namespace uagent {

// The one marker for text cut from the middle.
inline std::string OmittedMarker(size_t bytes) {
  return "\n... [" + std::to_string(bytes) + " bytes omitted] ...\n";
}

// Bounded process output preserving a stable prefix and the newest suffix.
class HeadTailBuffer {
 public:
  explicit HeadTailBuffer(size_t max_bytes = size_t{1024} * 1024)
      : head_budget_(max_bytes / 2), tail_budget_(max_bytes - head_budget_) {}

  void Push(std::string_view bytes) {
    size_t head = std::min(head_budget_ - head_.size(), bytes.size());
    head_.append(bytes.substr(0, head));
    bytes.remove_prefix(head);
    if (bytes.empty()) return;
    if (tail_budget_ == 0) {
      omitted_ += bytes.size();
      return;
    }
    if (bytes.size() >= tail_budget_) {
      omitted_ += tail_.size() + bytes.size() - tail_budget_;
      tail_.assign(bytes.end() - static_cast<std::ptrdiff_t>(tail_budget_),
                   bytes.end());
      return;
    }
    // The tail may run to twice its budget before its oldest bytes go, so
    // each byte is moved at most once: amortised O(1) per byte pushed.
    tail_.append(bytes.data(), bytes.size());
    if (tail_.size() > 2 * tail_budget_) {
      size_t excess = tail_.size() - tail_budget_;
      tail_.erase(0, excess);
      omitted_ += excess;
    }
  }

  // A cut never splits a codepoint, so the text serializes as it is.
  std::string Snapshot() const {
    const std::string_view tail = Tail();
    size_t omitted = omitted_ + tail_.size() - tail.size();
    if (head_.empty() && tail.empty() && omitted == 0) return "";
    if (omitted == 0) return head_ + std::string(tail);
    const size_t head_end = Utf8BoundaryBefore(head_, head_.size());
    const size_t tail_start =
        Utf8BoundaryAfter(tail_, tail_.size() - tail.size());
    omitted +=
        head_.size() - head_end + tail_start - (tail_.size() - tail.size());
    return head_.substr(0, head_end) + OmittedMarker(omitted) +
           tail_.substr(tail_start);
  }

  // The newest non-empty line, for a status row that has one row to spend on
  // a process still running. Scans backwards over a bounded window instead of
  // building Snapshot(), which is a megabyte the caller would throw away.
  std::string TailLine(size_t cap = 160) const {
    std::string line = LastLine(Tail(), cap);
    return line.empty() ? LastLine(head_, cap) : line;
  }

  std::string Drain() {
    std::string output = Snapshot();
    head_.clear();
    tail_.clear();
    omitted_ = 0;
    return output;
  }

 private:
  static bool Blank(char c) {
    return c == '\n' || c == '\r' || c == ' ' || c == '\t';
  }

  // Trailing blanks are skipped rather than answered with an empty line: a
  // process that just printed a newline has not stopped saying what it was
  // saying. The window bounds a producer that emits one enormous line or a
  // great many blank ones -- the cost stays the same either way.
  static std::string LastLine(std::string_view text, size_t cap) {
    static constexpr size_t kWindow = 4096;
    size_t limit = text.size() > kWindow ? text.size() - kWindow : 0;
    size_t end = text.size();
    while (end > limit && Blank(text[end - 1])) --end;
    if (end == limit) return std::string();
    size_t begin = end;
    while (begin > limit && text[begin - 1] != '\n' &&
           text[begin - 1] != '\r') {
      --begin;
    }
    while (begin < end && Blank(text[begin])) ++begin;
    // Cut from the front: the head of a progress line is what names the work,
    // and the caller bounds it again for its own width.
    return Utf8Prefix(std::string(text.substr(begin, end - begin)), cap);
  }

  // The newest tail_budget_ bytes: what the buffer keeps.
  std::string_view Tail() const {
    return std::string_view(tail_).substr(
        tail_.size() > tail_budget_ ? tail_.size() - tail_budget_ : 0);
  }

  size_t head_budget_;
  size_t tail_budget_;
  std::string head_;
  std::string tail_;
  size_t omitted_ = 0;
};

// Over-cap text keeps its head and its tail, where the beginning and the
// errors are; the middle is what a reader can most afford to lose. The cap
// includes the marker, and no codepoint is split.
inline std::string HeadTail(std::string text, size_t cap) {
  if (text.size() <= cap) return text;
  if (cap <= 3) return std::string(cap, '.');
  // The marker names the bytes it hides, and its own width decides how many
  // that is, so the count is settled by a few passes.
  size_t omitted = text.size() - cap;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const std::string marker = OmittedMarker(omitted);
    if (marker.size() >= cap) break;
    const size_t keep = cap - marker.size();
    const size_t head_end = Utf8BoundaryBefore(text, keep / 2);
    const size_t tail_start = std::max(
        head_end, Utf8BoundaryAfter(text, text.size() - (keep - keep / 2)));
    if (tail_start - head_end == omitted) {
      return text.substr(0, head_end) + marker + text.substr(tail_start);
    }
    omitted = tail_start - head_end;
  }
  return Utf8Prefix(std::move(text), cap - 3) + "...";
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_OUTPUT_BUFFER_H_
