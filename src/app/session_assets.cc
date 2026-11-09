// Copyright 2026 Timon Gentzsch

#include <filesystem>
#include <string>
#include <utility>

#include "include/app/session_host.h"
#include "include/core/fs.h"

namespace uagent::session {

std::string SessionHost::AssetPath(const std::string& id) const {
  std::lock_guard lock(mutex_);
  auto found = sessions_.find(id);
  return found == sessions_.end() ? std::string{} : found->second->path;
}

std::string SessionHost::Folder(const std::string& id) const {
  std::lock_guard lock(mutex_);
  auto found = sessions_.find(id);
  if (found == sessions_.end()) return {};
  return found->second->cwd;
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
        found->second->status == SessionStatus::kDeleting) {
      return {{}, "session changed while storing attachment", 409};
    }
  }
  return stored;
}

std::string SessionHost::DeleteShared(const std::string& session_id,
                                      const std::string& asset) {
  const std::string path = AssetPath(session_id);
  if (path.empty() || asset.empty() ||
      asset.find_first_not_of("0123456789abcdef") != std::string::npos) {
    return "not found";
  }
  const std::string stem = path + kSessionAssetsSuffix + "/" + asset;
  {
    std::unique_lock guard = assets_.GuardMutation();
    const AssetRecord found = ReadAssetRecord(stem);
    if (!JsonValue(found.record, "shared", false)) return "not found";
    std::error_code ec;
    if (!found.extension.empty()) {
      std::filesystem::remove(stem + found.extension, ec);
    }
    if (ec || !std::filesystem::remove(stem + ".json", ec)) {
      return "cannot delete the file";
    }
  }
  assets_.Invalidate();
  return "";
}

}  // namespace uagent::session
