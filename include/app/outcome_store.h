// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_OUTCOME_STORE_H_
#define UAGENT_INCLUDE_APP_OUTCOME_STORE_H_
// Worker-command acknowledgement tracking. Each command forwarded to a
// session worker registers a pending receipt keyed by worker request id;
// the worker's outcome frame resolves it, a disconnect fails everything the
// session still awaits, and the host reads its own view without touching
// worker state. Bounded: the oldest completed receipt is evicted first, and
// a log with nothing completable refuses rather than forgets a live
// command. Bodies live in src/app/outcome_store.cc.

#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "include/core/json.h"

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
  std::map<std::string, json> outcomes_;
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_OUTCOME_STORE_H_
