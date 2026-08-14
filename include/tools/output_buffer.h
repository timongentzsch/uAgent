// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_OUTPUT_BUFFER_H_
#define UAGENT_INCLUDE_TOOLS_OUTPUT_BUFFER_H_

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace uagent {

// Bounded process output preserving a stable prefix and the newest suffix.
class HeadTailBuffer {
 public:
  explicit HeadTailBuffer(size_t max_bytes = 1024 * 1024)
      : head_budget_(max_bytes / 2),
        tail_budget_(max_bytes - head_budget_) {}

  void Push(std::string_view bytes) {
    size_t head = std::min(head_budget_ - head_.size(), bytes.size());
    head_.append(bytes.data(), head);
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

  std::string Drain() {
    std::string output = Snapshot();
    head_.clear();
    tail_.clear();
    omitted_ = 0;
    return output;
  }

  size_t RetainedBytes() const { return head_.size() + tail_.size(); }
  size_t OmittedBytes() const { return omitted_; }

 private:
  size_t head_budget_;
  size_t tail_budget_;
  std::string head_;
  std::string tail_;
  size_t omitted_ = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_OUTPUT_BUFFER_H_
