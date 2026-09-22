// Copyright 2026 Timon Gentzsch

#include "include/app/private_store.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <string>
#include <utility>

#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

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
  return AtomicWriteFile(path_, bytes, kPrivateFileMode,
                         /*preserve_mode=*/false, error);
}

}  // namespace uagent
