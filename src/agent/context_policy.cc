// Copyright 2026 Timon Gentzsch

#include "include/agent/context_policy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "include/core/checked.h"

namespace uagent {
namespace {

int64_t EstimatedTokens(size_t message_bytes, size_t schema_bytes,
                        bool native_tools) {
  size_t bytes = message_bytes;
  if (native_tools) {
    auto total = CheckedAdd(bytes, schema_bytes);
    bytes = total.value_or(std::numeric_limits<size_t>::max());
  }
  size_t tokens = bytes / 4;
  return tokens > static_cast<size_t>(std::numeric_limits<int64_t>::max())
             ? std::numeric_limits<int64_t>::max()
             : static_cast<int64_t>(tokens);
}

}  // namespace

int64_t ContextPolicy::Used(size_t message_bytes, size_t schema_bytes,
                            bool native_tools) const {
  return reported_tokens_ > 0
             ? reported_tokens_
             : EstimatedTokens(message_bytes, schema_bytes, native_tools);
}

void ContextPolicy::SetReported(int64_t tokens) {
  reported_tokens_ = std::max(int64_t{0}, tokens);
}

void ContextPolicy::Reset() {
  reported_tokens_ = 0;
  last_hint_turn_ = 0;
  ignored_urgent_hints_ = 0;
}

void ContextPolicy::ResetUrgency() { ignored_urgent_hints_ = 0; }

void ContextPolicy::HintIssued(int64_t turn) { last_hint_turn_ = turn; }

ContextDecision ContextPolicy::Prepare(const ContextPolicyInput& input) {
  int64_t compact = std::clamp(input.compact_pct, int64_t{0}, int64_t{100});
  int64_t assess = std::clamp(input.checkpoint_pct, int64_t{0}, int64_t{100});
  int64_t urgent = std::clamp(input.urgent_pct, assess, int64_t{100});
  int64_t reserve =
      input.context_window > 0
          ? std::min(input.max_output_tokens, input.context_window / 4)
          : 0;
  int64_t pending = static_cast<int64_t>(
      std::min(input.pending_bytes / 4,
               static_cast<size_t>(std::numeric_limits<int64_t>::max())));
  int64_t used =
      Used(input.message_bytes, input.schema_bytes, input.native_tools);
  int64_t projected =
      used > std::numeric_limits<int64_t>::max() - pending - reserve
          ? std::numeric_limits<int64_t>::max()
          : used + pending + reserve;
  int64_t pct = 0;
  if (input.context_window > 0) {
    pct = projected > std::numeric_limits<int64_t>::max() / 100
              ? 100
              : projected * 100 / std::max(int64_t{1}, input.context_window);
  } else if (input.request_bytes > 0) {
    auto bytes = CheckedAdd(input.message_bytes, input.pending_bytes);
    size_t request_bytes = static_cast<size_t>(input.request_bytes);
    pct = !bytes || *bytes > request_bytes
              ? 100
              : static_cast<int64_t>(*bytes * 100 / request_bytes);
  }
  pct = std::max(int64_t{0}, pct);
  if (pct < urgent) ignored_urgent_hints_ = 0;

  if (compact > 0 && pct >= compact) {
    return {ContextAction::kCompact, pct, false};
  }
  if (!input.checkpoint_enabled || assess == 0 || pct < assess ||
      input.message_count < 2 ||
      (last_hint_turn_ > 0 && input.turn - last_hint_turn_ < 3)) {
    return {ContextAction::kNone, pct, false};
  }
  if (pct >= urgent) {
    if (++ignored_urgent_hints_ > 2) {
      return {ContextAction::kCompact, pct, true};
    }
    return {ContextAction::kUrgentCheckpoint, pct, false};
  }
  return {ContextAction::kCheckpoint, pct, false};
}

}  // namespace uagent
