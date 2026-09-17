// Copyright 2026 Timon Gentzsch

#include "include/app/replay_log.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/app/session.h"
#include "include/core/json.h"

namespace uagent::session {

ReplayLog::ReplayLog(size_t byte_limit, size_t event_limit)
    : byte_limit_(byte_limit), event_limit_(event_limit) {}

HostReplay ReplayLog::Publish(const std::string& epoch,
                              const std::string& session,
                              const std::string& generation, json value,
                              bool run_owned) {
  const std::string type = JsonValue(value, "type", "");
  const std::string kind = JsonValue(value, "kind", "");
  HostNotice notice;
  notice.session = session;
  if (type == "turn.completed" || type == "approval.requested" ||
      type == "error" || kind == "error") {
    notice.attention_id = epoch + ":" + std::to_string(sequence_ + 1);
    value["attention_id"] = notice.attention_id;
  }
  notice.wake = kind == "scheduled.changed" || kind == "management.changed" ||
                kind == "metadata" || kind == "activated" ||
                kind == "deactivated" || run_owned;
  if (kind == "deleted") notices_.erase(session);
  value["epoch"] = epoch;
  value["sequence"] = ++sequence_;
  value["session_id"] = session;
  value["generation"] = generation;
  value["v"] = kProtocol;
  HostReplay published{sequence_, JsonDump(value)};
  replay_bytes_ += published.frame.size();
  replay_.push_back(published);
  while (replay_bytes_ > byte_limit_ || replay_.size() > event_limit_) {
    replay_bytes_ -= replay_.front().frame.size();
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
  return published;
}

ReplayBatch ReplayLog::Read(uint64_t next, bool valid,
                            uint64_t watermark) const {
  ReplayBatch batch;
  batch.cursor = sequence_;
  batch.reset = !valid || next > sequence_ ||
                (!replay_.empty() && next + 1 < replay_.front().sequence);
  if (batch.reset) return batch;
  for (const HostReplay& event : replay_) {
    if (event.sequence > next && event.sequence <= watermark) {
      batch.events.push_back(event);
    }
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
