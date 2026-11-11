// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_ASSET_STORE_H_
#define UAGENT_INCLUDE_APP_ASSET_STORE_H_
// Session attachment storage: quotas and the claim/commit handshake, under
// its own mutex so uploads never serialize on the host lock.

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/thread_annotations.h"

namespace uagent::session {

// Upload quotas: one file, one message, one session, all sessions.
inline constexpr size_t kUploadBytes = MiB(8);
inline constexpr size_t kUploadCount = 8;
inline constexpr size_t kSessionAssetBytes = MiB(64);
inline constexpr size_t kGlobalAssetBytes = MiB(512);
// An asset's record (`<id>.json` beside its bytes), small by construction.
inline constexpr size_t kAssetRecordBytes = KiB(1);

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
  // Persists one upload into session_path + ".assets". `committed`: kept for
  // the session's life. `tool_copy`: a display copy, evicted oldest first.
  // `shared`: listed in the Library.
  AssetStoreResult Store(const std::string& session_path,
                         const std::string& bytes, std::string name,
                         bool committed = false, bool tool_copy = false,
                         bool shared = false);
  // Resolves attachment ids into command["attachments"]. A non-empty `gate`
  // result aborts before any commit; `receipt` takes what Unclaim rolls back.
  std::string Claim(const std::string& session_path, const json& ids,
                    json& command, const std::function<std::string()>& gate,
                    AssetClaim& receipt);
  void Unclaim(AssetClaim& receipt);
  // Guards a session-file rename or delete against attachment scans.
  std::unique_lock<std::mutex> GuardMutation();
  // Forces the next Store to rescan the quotas, after a session was deleted.
  void Invalidate();

 private:
  std::mutex mutex_;
  std::chrono::steady_clock::time_point scanned_ UAGENT_GUARDED_BY(mutex_){};
  size_t bytes_ UAGENT_GUARDED_BY(mutex_) = 0;
};

// The record at `stem` (path without extension), null when none; `extension`
// is empty when the record names one its type cannot have.
struct AssetRecord {
  json record;
  std::string extension;
};
AssetRecord ReadAssetRecord(const std::string& stem);

// The process's one store, so its quota scan is shared across callers.
AssetStore& SessionAssets();

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_ASSET_STORE_H_
