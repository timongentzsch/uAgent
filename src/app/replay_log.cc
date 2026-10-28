// Copyright 2026 Timon Gentzsch

#include "include/app/replay_log.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/app/session.h"
#include "include/core/events.h"
#include "include/core/json.h"

namespace uagent::session {

ReplayLog::ReplayLog(size_t byte_limit, size_t event_limit)
    : byte_limit_(byte_limit), event_limit_(event_limit) {}

void ReplayLog::Publish(const std::string& epoch, const std::string& session,
                        const std::string& generation, json value,
                        bool run_owned) {
  const std::string type = JsonValue(value, "type", "");
  const std::string kind = JsonValue(value, "kind", "");
  HostNotice notice;
  notice.session = session;
  // A decision routed to a thread's panel reaches a person only once
  // the panel yields it (approval.escalated).
  const bool routed = RoutedToPanel(JsonValue(value, "data", json::object()));
  if (type == AppType(EventId::kTurnCompleted) ||
      ((type == AppType(EventId::kApprovalRequested) ||
        type == "ask.requested") &&
       !routed) ||
      type == "approval.escalated" || type == AppType(EventId::kError) ||
      kind == "error") {
    notice.attention_id = epoch + ":" + std::to_string(sequence_ + 1);
    value["attention_id"] = notice.attention_id;
  }
  // A run's own frames wake the scheduler, except the ones a turn streams by
  // the hundred: they change nothing it reads.
  const bool streamed = type == AppType(EventId::kAnswerDelta) ||
                        type == AppType(EventId::kReasoningDelta) ||
                        type == AppType(EventId::kUsageUpdated) ||
                        type == AppType(EventId::kToolArguments) ||
                        type == AppType(EventId::kHostedToolActivity) ||
                        type == AppType(EventId::kHttpExchange);
  notice.wake = kind == "scheduled.changed" || kind == "management.changed" ||
                kind == "spaces.changed" || kind == "metadata" ||
                kind == "activated" || kind == "deactivated" ||
                (run_owned && !streamed);
  if (kind == "deleted") notices_.erase(session);
  value["epoch"] = epoch;
  value["sequence"] = ++sequence_;
  StampFrame(value, session, generation);
  replay_.push_back(
      {sequence_, std::make_shared<const std::string>(
                      "id: " + epoch + ":" + std::to_string(sequence_) +
                      "\nevent: update\ndata: " + JsonDump(value) + "\n\n")});
  replay_bytes_ += replay_.back().frame->size();
  while (replay_bytes_ > byte_limit_ || replay_.size() > event_limit_) {
    replay_bytes_ -= replay_.front().frame->size();
    replay_.pop_front();
  }
  if (!notice.attention_id.empty() || notice.wake) {
    auto& pending = notices_[notice.session];
    pending.session = notice.session;
    pending.wake |= notice.wake;
    if (!notice.attention_id.empty()) {
      pending.attention_id = std::move(notice.attention_id);
    }
  }
}

ReplayBatch ReplayLog::Read(uint64_t next, bool valid, uint64_t watermark,
                            size_t byte_budget) const {
  ReplayBatch batch;
  batch.cursor = sequence_;
  batch.reset = !valid || next > sequence_ ||
                (!replay_.empty() && next + 1 < replay_.front().sequence);
  if (batch.reset) return batch;
  // Sequences are consecutive, so the first unread frame is found by index.
  size_t bytes = 0;
  for (size_t index = replay_.empty() ? 0 : next + 1 - replay_.front().sequence;
       index < replay_.size(); ++index) {
    const HostReplay& event = replay_[index];
    if (event.sequence > watermark || bytes >= byte_budget) break;
    batch.events.push_back(event);
    bytes += event.frame->size();
  }
  return batch;
}

std::vector<HostNotice> ReplayLog::TakeNotices() {
  std::vector<HostNotice> notices;
  notices.reserve(notices_.size());
  for (auto& [session, notice] : notices_) {
    notices.push_back(std::move(notice));
  }
  notices_.clear();
  return notices;
}

}  // namespace uagent::session
