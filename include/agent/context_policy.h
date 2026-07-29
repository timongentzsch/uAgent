// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_CONTEXT_POLICY_H_
#define UAGENT_INCLUDE_AGENT_CONTEXT_POLICY_H_

#include <cstddef>
#include <cstdint>

namespace uagent {

enum class ContextAction {
  kNone,
  kCompact,
  kCheckpoint,
  kUrgentCheckpoint,
};

struct ContextPolicyInput {
  size_t message_bytes = 0;
  size_t pending_bytes = 0;
  size_t message_count = 0;
  size_t schema_bytes = 0;
  bool native_tools = false;
  int64_t context_window = 0;
  int64_t request_bytes = 0;
  int64_t max_output_tokens = 0;
  int64_t compact_pct = 0;
  int64_t checkpoint_pct = 0;
  int64_t urgent_pct = 0;
  bool checkpoint_enabled = true;
  int64_t turn = 0;
};

struct ContextDecision {
  ContextAction action = ContextAction::kNone;
  int64_t projected_pct = 0;
  bool forced = false;
};

class ContextPolicy {
 public:
  int64_t Used(size_t message_bytes, size_t schema_bytes,
               bool native_tools) const;
  void SetReported(int64_t tokens);
  void Reset();
  void ResetUrgency();
  void HintIssued(int64_t turn);
  ContextDecision Prepare(const ContextPolicyInput& input);

 private:
  int64_t reported_tokens_ = 0;
  int64_t last_hint_turn_ = 0;
  int64_t ignored_urgent_hints_ = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_CONTEXT_POLICY_H_
