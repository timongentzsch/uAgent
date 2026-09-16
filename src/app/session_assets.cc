// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_view.h"
#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/app/session.h"
#include "include/app/session_host.h"
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
namespace {
struct AssetUsage {
  size_t bytes = 0, count = 0;
  bool valid = true;
};

AssetUsage InspectAssets(const std::string& folder, bool cleanup) {
  AssetUsage usage;
  std::error_code ec;
  if (!std::filesystem::is_directory(
          std::filesystem::symlink_status(folder, ec))) {
    return usage;
  }
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (!std::filesystem::is_regular_file(entry.symlink_status(ec))) {
      usage.valid = false;
      break;
    }
    if (entry.path().extension() == ".json") continue;
    if (cleanup && std::filesystem::file_time_type::clock::now() -
                           entry.last_write_time(ec) >
                       std::chrono::hours(24)) {
      auto metadata = entry.path();
      metadata.replace_extension(".json");
      std::string bytes, error;
      json record;
      if (ReadRegularFile(metadata.string(), 1024, bytes, error)) {
        record = json::parse(bytes, nullptr, false);
      }
      if (!JsonValue(record, "committed", false)) {
        std::filesystem::remove(entry.path(), ec);
        if (!ec) {
          std::filesystem::remove(metadata, ec);
          continue;
        }
      }
    }
    uintmax_t bytes = entry.file_size(ec);
    if (ec || bytes > kGlobalAssetBytes ||
        usage.bytes > kGlobalAssetBytes - bytes) {
      usage.valid = false;
      break;
    }
    usage.bytes += static_cast<size_t>(bytes);
    if (++usage.count > 64) {
      usage.valid = false;
      break;
    }
  }
  return usage;
}

// Negative or unparsable offsets read from the start; callers pass paths, not
// positions, so a clamp is the whole policy.
}  // namespace
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
  std::lock_guard assets(asset_mutex_);
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(session_id);
    if (found == sessions_.end() || found->second->path != session_path ||
        found->second->status == "deleting") {
      return {{}, "session changed while storing attachment", 409};
    }
  }
  const std::string folder = session_path + ".assets";
  if (!EnsurePrivateDirectory(folder)) {
    return {{}, "cannot create asset storage", 500};
  }
  if (std::chrono::steady_clock::now() - assets_scanned_ >=
      std::chrono::minutes(1)) {
    assets_scanned_ = std::chrono::steady_clock::now();
    asset_bytes_ = 0;
    std::error_code ec;
    std::vector<std::filesystem::path> folders{UagentDir(kHistoryDir)};
    for (const auto& entry :
         std::filesystem::directory_iterator(folders.front(), ec)) {
      if (std::filesystem::is_directory(entry.symlink_status(ec)) &&
          !entry.path().string().ends_with(".assets")) {
        folders.push_back(entry.path());
      }
      if (folders.size() > 4096) {
        asset_bytes_ = kGlobalAssetBytes;
        break;
      }
    }
    size_t scanned = 0;
    for (const auto& directory : folders) {
      for (const auto& entry :
           std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.path().string().ends_with(".json.assets") ||
            !std::filesystem::is_directory(entry.symlink_status(ec))) {
          continue;
        }
        AssetUsage usage = InspectAssets(entry.path().string(), true);
        if (++scanned > 4096 || !usage.valid ||
            usage.bytes > kGlobalAssetBytes - asset_bytes_) {
          asset_bytes_ = kGlobalAssetBytes;
          break;
        }
        asset_bytes_ += usage.bytes;
      }
      if (asset_bytes_ >= kGlobalAssetBytes) break;
    }
  }
  AssetUsage usage = InspectAssets(folder, true);
  if (!usage.valid || usage.count >= 64 ||
      bytes.size() >
          kSessionAssetBytes - std::min(usage.bytes, kSessionAssetBytes) ||
      bytes.size() >
          kGlobalAssetBytes - std::min(asset_bytes_, kGlobalAssetBytes)) {
    return {{}, "attachment storage limit reached", 413};
  }
  std::string mime = RasterMime(bytes);
  const bool image = !mime.empty();
  name = Utf8Prefix(std::filesystem::path(name).filename().string(), 240);
  if (name.empty()) {
    name = image ? "image" + ImageExtension(mime) : "attachment";
  }
  for (char& ch : name) {
    if (static_cast<unsigned char>(ch) < 32) ch = '_';
  }
  if (mime.empty()) {
    mime = AttachmentMime(name);
    if (mime.starts_with("image/")) mime = "application/octet-stream";
  }
  std::string id = RandomToken(16), stem = folder + "/" + id;
  if (id.empty() || !ToolWritePrivateFile(stem + ".data", bytes).Ok()) {
    return {{}, "cannot persist upload", 500};
  }
  asset_bytes_ += bytes.size();
  if (!ToolWritePrivateFile(stem + ".json", JsonDump({{"mime", mime},
                                                      {"name", name},
                                                      {"image", image},
                                                      {"extension", ".data"},
                                                      {"bytes", bytes.size()},
                                                      {"committed", false}}))
           .Ok()) {
    return {{}, "cannot persist upload metadata", 500};
  }
  return {{{"id", id},
           {"name", name},
           {"mime", mime},
           {"image", image},
           {"bytes", bytes.size()}},
          {},
          200};
}

}  // namespace uagent::session
