// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_REPLAY_LOG_H_
#define UAGENT_INCLUDE_APP_REPLAY_LOG_H_
// Transport-independent host event ordering and bounded reconnect replay.
// The HTTP adapter serializes these frames but does not own their sequence.
// The host owns the lock; every method below runs with it held, so this
// class carries data but no mutex or condition variable. Bodies live in
// src/app/replay_log.cc.

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "include/app/session.h"
#include "include/core/json.h"

namespace uagent::session {

struct HostReplay {
  uint64_t sequence = 0;
  std::string frame;
};

struct ReplayBatch {
  uint64_t cursor = 0;
  bool reset = false;
  std::vector<HostReplay> events;
};

struct HostNotice {
  std::string attention_id, session;
  bool wake = false;
};

class ReplayLog {
 public:
  ReplayLog(size_t byte_limit, size_t event_limit);

  // Appends one host event, evicting oldest-first past either bound.
  // run_owned mirrors the old sessions_ lookup: a live scheduled run wakes
  // the scheduler even for otherwise quiet kinds.
  HostReplay Publish(const std::string& epoch, const std::string& session,
                     const std::string& generation, json value, bool run_owned);
  uint64_t Cursor() const { return sequence_; }
  ReplayBatch Read(uint64_t next, bool valid, uint64_t watermark) const;
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
