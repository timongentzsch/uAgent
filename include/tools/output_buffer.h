// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_OUTPUT_BUFFER_H_
#define UAGENT_INCLUDE_TOOLS_OUTPUT_BUFFER_H_

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "include/core/strings.h"

namespace uagent {

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
    tail_.append(bytes.data(), bytes.size());
    if (tail_.size() > tail_budget_) {
      size_t excess = tail_.size() - tail_budget_;
      tail_.erase(0, excess);
      omitted_ += excess;
    }
  }

  std::string Snapshot() const {
    if (head_.empty() && tail_.empty() && omitted_ == 0) return "";
    std::string output = head_;
    if (omitted_ > 0) {
      output += "\n... " + std::to_string(omitted_) + " bytes omitted ...\n";
    }
    output += tail_;
    return output;
  }

  // The newest non-empty line, for a status row that has one row to spend on
  // a process still running. Scans backwards over a bounded window instead of
  // building Snapshot(), which is a megabyte the caller would throw away.
  std::string TailLine(size_t cap = 160) const {
    std::string line = LastLine(tail_, cap);
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
  static std::string LastLine(const std::string& text, size_t cap) {
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
    return Utf8Prefix(text.substr(begin, end - begin), cap);
  }

  size_t head_budget_;
  size_t tail_budget_;
  std::string head_;
  std::string tail_;
  size_t omitted_ = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_OUTPUT_BUFFER_H_
