// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_REPLAY_LOG_H_
#define UAGENT_INCLUDE_APP_REPLAY_LOG_H_
// Host event ordering and bounded reconnect replay. Every method runs with
// the host's lock held: the class has no mutex of its own.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "include/app/session.h"
#include "include/core/json.h"

namespace uagent::session {

struct HostReplay {
  uint64_t sequence = 0;
  // `id: EPOCH:SEQUENCE`, `event: update` and the frame as `data`.
  std::shared_ptr<const std::string> frame;
};

struct ReplayBatch {
  uint64_t cursor = 0;
  bool reset = false;
  std::vector<HostReplay> events;
};

struct HostNotice {
  // `reason` is what raised the notice: approval, question, error or
  // finished; `title` is the conversation's, filled in by the host.
  std::string attention_id, session, reason, title;
  bool wake = false;
};

class ReplayLog {
 public:
  ReplayLog(size_t byte_limit, size_t event_limit);

  // Appends one host event, evicting oldest-first past either bound. With
  // `run_owned` (a live scheduled run) the frame also wakes the scheduler.
  void Publish(const std::string& epoch, const std::string& session,
               const std::string& generation, json value, bool run_owned);
  uint64_t Cursor() const { return sequence_; }
  // Events after `next` up to `watermark`, stopping once `byte_budget` of
  // frames is reached: a reader sends one batch at a time.
  ReplayBatch Read(uint64_t next, bool valid, uint64_t watermark,
                   size_t byte_budget) const;
  bool HasNotices() const { return !notices_.empty(); }
  std::vector<HostNotice> TakeNotices();

 private:
  size_t byte_limit_;
  size_t event_limit_;
  size_t replay_bytes_ = 0;
  uint64_t sequence_ = 0;
  std::deque<HostReplay> replay_;
  std::map<std::string, HostNotice> notices_;
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_REPLAY_LOG_H_
