// Copyright 2026 Timon Gentzsch

#include "include/core/private_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

json ReadJsonFile(const std::string& path, size_t byte_limit) {
  std::string bytes, error;
  if (!ReadRegularFile(path, byte_limit, bytes, error)) {
    return json(json::value_t::discarded);
  }
  return json::parse(bytes, nullptr, false);
}

PrivateJsonStore::PrivateJsonStore(const std::string& filename, json empty,
                                   size_t byte_limit, std::string& error)
    : path_(UagentDir(kConfigDir) + "/" + filename),
      lock_(
          open((path_ + ".lock").c_str(), O_CREAT | O_RDWR, kPrivateFileMode)),
      data_(std::move(empty)),
      byte_limit_(byte_limit) {
  if (!lock_) {
    error = "cannot open private settings lock";
    return;
  }
  fchmod(lock_.Get(), kPrivateFileMode);
  if (!LockFileExclusive(lock_.Get())) {
    error = "cannot lock private settings";
    return;
  }
  if (!PathExists(path_)) {
    ready_ = true;
    return;
  }
  std::string bytes;
  if (!ReadRegularFile(path_, byte_limit, bytes, error)) return;
  json parsed = json::parse(bytes, nullptr, false);
  if (parsed.is_discarded()) {
    error = "private settings file contains invalid JSON";
    return;
  }
  data_ = std::move(parsed);
  ready_ = true;
}

bool PrivateJsonStore::Save(std::string& error) const {
  if (!ready_) {
    error = "private settings are unavailable";
    return false;
  }
  const std::string bytes = JsonDump(data_, 2) + "\n";
  if (bytes.size() > byte_limit_) {
    error = "private settings exceed the storage limit";
    return false;
  }
  return WritePrivateFile(path_, bytes, error);
}

json ReadPrivateArtifact(const std::string& path, size_t offset) {
  const std::filesystem::path file_path(path);
  if (path.empty()) return {{"error", "retained body unavailable"}};
  // Either a retained artifact, or a diff kept with a session's edits.
  auto base = CanonicalAccessPath(file_path.parent_path());
  const bool diff =
      file_path.filename().string().starts_with("diff-") &&
      base.string().ends_with(std::string(".json") + kSessionEditsSuffix) &&
      PathWithin(base, CanonicalAccessPath(UagentDir(kHistoryDir)));
  if (!diff && base != CanonicalAccessPath(UagentDir(kArtifactsDir))) {
    return {{"error", "retained body unavailable"}};
  }
  size_t size = 0;
  Fd file =
      OpenOwnedRegular((base / file_path.filename()).string(), SIZE_MAX, &size);
  if (!file) return {{"error", "retained body unavailable"}};
  offset = std::min(offset, size);
  // Look ahead so the page boundary can be checked against the next byte.
  std::string text(std::min(size - offset, kTextPageBytes + 4), '\0');
  ssize_t bytes;
  do {
    bytes =
        pread(file.Get(), text.data(), text.size(), static_cast<off_t>(offset));
  } while (bytes < 0 && errno == EINTR);
  if (bytes < 0) return {{"error", "retained body read failed"}};
  text.resize(static_cast<size_t>(bytes));
  // JSON pages split only at UTF-8 boundaries, never inside a code point.
  const size_t start = Utf8BoundaryAfter(text, 0);
  const size_t end =
      Utf8BoundaryBefore(text, std::min(kTextPageBytes, text.size()));
  return {{"text", text.substr(start, end - start)},
          {"next", offset + end},
          {"bytes", size},
          {"more", offset + end < size}};
}

}  // namespace uagent
