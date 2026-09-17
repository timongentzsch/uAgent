// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_ASSET_STORE_H_
#define UAGENT_INCLUDE_APP_ASSET_STORE_H_
// Session attachment storage: quota-scanned asset directories plus the
// claim/commit handshake that hands asset records to worker commands.
// The host owns session identity (paths, liveness); this class owns bytes,
// quotas and record commits under its own mutex, so attachment backpressure
// never serializes on the host lock. Bodies live in src/app/asset_store.cc.

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"

namespace uagent::session {

struct AssetStoreResult {
  json value;
  std::string error;
  int status = 200;
};

// Records committed by Claim, for rollback when the worker never takes them.
struct AssetClaim {
  std::vector<std::pair<std::string, json>> records;
};

class AssetStore {
 public:
  // Persists one upload into session_path + ".assets". The caller checks
  // session liveness before and after; uncommitted uploads older than a day
  // are reaped by the quota scan, so a lost race only costs bytes, not truth.
  AssetStoreResult Store(const std::string& session_path,
                         const std::string& bytes, std::string name);
  // Resolves attachment id claims into command["attachments"], runs gate
  // (the host's session-liveness check) before committing anything, then
  // size-checks and commits. gate's non-empty return aborts uncommitted.
  // Committed records land in receipt for Unclaim on dispatch failure.
  std::string Claim(const std::string& session_path, const json& ids,
                    json& command, std::function<std::string()> gate,
                    AssetClaim& receipt);
  void Unclaim(AssetClaim& receipt);
  // Guards a session-file mutation (rename/delete) that attachment scans
  // must not interleave with. Replaces direct use of the old host asset
  // mutex at the one call site that mutates session files outside Store.
  std::unique_lock<std::mutex> GuardMutation();
  // A deleted session's quota contribution is gone with its directory.
  void Invalidate();

 private:
  std::mutex mutex_;
  std::chrono::steady_clock::time_point scanned_{};
  size_t bytes_ = 0;
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_ASSET_STORE_H_
