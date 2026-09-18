// Copyright 2026 Timon Gentzsch

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "include/app/session_host.h"

namespace uagent::session {

std::string SessionHost::AssetPath(const std::string& id) const {
  std::lock_guard lock(mutex_);
  auto found = sessions_.find(id);
  return found == sessions_.end() ? std::string{} : found->second->path;
}

AssetStoreResult SessionHost::StoreAsset(const std::string& session_id,
                                         const std::string& bytes,
                                         std::string name) {
  std::string session_path;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(session_id);
    if (found == sessions_.end()) {
      return {{}, "unknown session", 404};
    }
    session_path = found->second->path;
  }
  AssetStoreResult stored = assets_.Store(session_path, bytes, std::move(name));
  if (!stored.error.empty()) return stored;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(session_id);
    if (found == sessions_.end() || found->second->path != session_path ||
        found->second->status == "deleting") {
      return {{}, "session changed while storing attachment", 409};
    }
  }
  return stored;
}

}  // namespace uagent::session
