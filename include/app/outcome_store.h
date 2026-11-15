// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_OUTCOME_STORE_H_
#define UAGENT_INCLUDE_APP_OUTCOME_STORE_H_
// Acknowledgements of commands forwarded to a session worker. Bounded: the
// oldest completed receipt goes first, and a log of pending ones refuses.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "include/core/json.h"
#include "include/core/thread_annotations.h"

namespace uagent::session {

struct HostSession;

class OutcomeStore {
 public:
  json SendCommand(const std::shared_ptr<HostSession>& session, json command,
                   const std::string& worker_request,
                   const std::string& client_request, bool& dispatched);
  json CommandOutcome(const std::string& worker_request,
                      const std::string& client_request) const;
  size_t PendingCommands(const HostSession& session) const;
  bool ResolveOutcome(HostSession& session, json& frame);
  void FailPending(HostSession& session);

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  // A caller pins its receipt while waiting, independently of cache eviction.
  std::map<std::string, std::shared_ptr<json>> outcomes_
      UAGENT_GUARDED_BY(mutex_);
  std::deque<std::string> order_ UAGENT_GUARDED_BY(mutex_);
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_OUTCOME_STORE_H_
