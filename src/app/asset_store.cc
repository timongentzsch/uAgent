// Copyright 2026 Timon Gentzsch

#include "include/app/asset_store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/app/session.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
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
    if (++usage.count > kMaxSessionAssets) {
      usage.valid = false;
      break;
    }
  }
  return usage;
}

// Attachment display names travel in frames and land on disk-adjacent
// records: no path separators, no control bytes, bounded length.
bool ValidAssetName(const std::string& name) {
  if (name.size() > kAssetNameChars || name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    return false;
  }
  for (char ch : name) {
    const auto code = static_cast<unsigned char>(ch);
    if (code < 32 || code == 127) return false;
  }
  return true;
}

void UnclaimLocked(std::vector<std::pair<std::string, json>>& claims,
                   size_t count) {
  for (size_t i = 0; i < count && i < claims.size(); ++i) {
    claims[i].second["committed"] = false;
    ToolWritePrivateFile(claims[i].first, JsonDump(claims[i].second));
  }
}

}  // namespace

AssetStoreResult AssetStore::Store(const std::string& session_path,
                                   const std::string& bytes, std::string name) {
  std::lock_guard assets(mutex_);
  const std::string folder = session_path + ".assets";
  if (!EnsurePrivateDirectory(folder)) {
    return {{}, "cannot create asset storage", 500};
  }
  if (std::chrono::steady_clock::now() - scanned_ >= std::chrono::minutes(1)) {
    scanned_ = std::chrono::steady_clock::now();
    bytes_ = 0;
    std::error_code ec;
    std::vector<std::filesystem::path> folders{UagentDir(kHistoryDir)};
    for (const auto& entry :
         std::filesystem::directory_iterator(folders.front(), ec)) {
      if (std::filesystem::is_directory(entry.symlink_status(ec)) &&
          !entry.path().string().ends_with(".assets")) {
        folders.push_back(entry.path());
      }
      if (folders.size() > kMaxCatalogueEntries) {
        bytes_ = kGlobalAssetBytes;
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
        if (++scanned > kMaxCatalogueEntries || !usage.valid ||
            usage.bytes > kGlobalAssetBytes - bytes_) {
          bytes_ = kGlobalAssetBytes;
          break;
        }
        bytes_ += usage.bytes;
      }
      if (bytes_ >= kGlobalAssetBytes) break;
    }
  }
  AssetUsage usage = InspectAssets(folder, true);
  if (!usage.valid || usage.count >= kMaxSessionAssets ||
      bytes.size() >
          kSessionAssetBytes - std::min(usage.bytes, kSessionAssetBytes) ||
      bytes.size() > kGlobalAssetBytes - std::min(bytes_, kGlobalAssetBytes)) {
    return {{}, "attachment storage limit reached", 413};
  }
  std::string mime = RasterMime(bytes);
  if (mime.empty()) mime = SvgMime(bytes);
  const bool image = !mime.empty();
  name = Utf8Prefix(std::filesystem::path(name).filename().string(),
                    kAssetNameChars);
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
  bytes_ += bytes.size();
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

std::string AssetStore::Claim(const std::string& session_path, const json& ids,
                              json& command,
                              const std::function<std::string()>& gate,
                              AssetClaim& receipt) {
  std::lock_guard assets(mutex_);
  if (ids.size() > kUploadCount) return "too many attachments";
  if (!EnsurePrivateDirectory(session_path + ".assets")) {
    return "unsafe asset directory";
  }
  std::vector<std::pair<std::string, json>> claims;
  for (const json& claim : ids) {
    // Claims are bare asset IDs, or {id, name} objects carrying the
    // composer's display name (deduped/renamed labels). The display
    // name is stored on the record before commit, so the history echo
    // and the model payload agree with what the sender saw.
    std::string asset_id = claim.is_string() ? claim.get<std::string>()
                                             : JsonValue(claim, "id", "");
    std::string display = claim.is_object() ? JsonValue(claim, "name", "") : "";
    if (!OpaqueId(asset_id)) return "invalid asset ID";
    if (!display.empty() && !ValidAssetName(display)) {
      return "invalid attachment name";
    }
    std::string stem = session_path + ".assets/" + asset_id;
    std::string metadata, read_error;
    if (!ReadRegularFile(stem + ".json", 1024, metadata, read_error)) {
      return "attachment is unavailable";
    }
    json asset = json::parse(metadata, nullptr, false);
    std::string extension = JsonValue(
        asset, "extension", ImageExtension(JsonValue(asset, "mime", "")));
    if (extension.empty() ||
        (extension != ".data" &&
         extension != ImageExtension(JsonValue(asset, "mime", "")))) {
      return "invalid file asset";
    }
    if (!display.empty()) asset["name"] = display;
    claims.emplace_back(stem + ".json", asset);
    command["attachments"].push_back(
        {{"path", stem + extension},
         {"id", asset_id},
         {"name", JsonValue(asset, "name", "attachment" + extension)},
         {"mime", JsonValue(asset, "mime", "application/octet-stream")},
         {"image", JsonValue(asset, "image", extension != ".data")}});
  }
  if (std::string blocked = gate(); !blocked.empty()) {
    return blocked;
  }
  if (JsonDump(command).size() > kCommandBytes) {
    return "message and resolved attachments exceed the command limit";
  }
  size_t committed = 0;
  for (auto& [path, asset] : claims) {
    asset["committed"] = true;
    if (!ToolWritePrivateFile(path, JsonDump(asset)).Ok()) {
      UnclaimLocked(claims, committed);
      return "cannot claim attachment";
    }
    ++committed;
  }
  receipt.records = std::move(claims);
  return "";
}

void AssetStore::Unclaim(AssetClaim& receipt) {
  std::lock_guard assets(mutex_);
  UnclaimLocked(receipt.records, receipt.records.size());
  receipt.records.clear();
}

void AssetStore::Invalidate() {
  std::lock_guard assets(mutex_);
  scanned_ = {};
}

std::unique_lock<std::mutex> AssetStore::GuardMutation() {
  return std::unique_lock(mutex_);
}

}  // namespace uagent::session
