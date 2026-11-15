// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PRIVATE_STORE_H_
#define UAGENT_INCLUDE_CORE_PRIVATE_STORE_H_
// Reading and writing private JSON files and retained bodies under
// ~/.uagent: owner-only, size-bounded, and read as nothing when missing.

#include <cstddef>
#include <string>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent {

// The parsed file, or a discarded value when it is missing, no regular file,
// over `byte_limit` or not JSON.
json ReadJsonFile(const std::string& path, size_t byte_limit);

// One page, from `offset`, of a retained artifact or a session's kept diff;
// any other path answers an error.
json ReadPrivateArtifact(const std::string& path, size_t offset);

// A small owner-only JSON document, locked across worker processes and
// replaced atomically. Callers validate their own schema.
class PrivateJsonStore {
 public:
  PrivateJsonStore(const std::string& filename, json empty, size_t byte_limit,
                   std::string& error);

  bool Ready() const { return ready_; }
  json& Data() { return data_; }
  const json& Data() const { return data_; }
  bool Save(std::string& error) const;

 private:
  std::string path_;
  Fd lock_;
  json data_;
  size_t byte_limit_;
  bool ready_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PRIVATE_STORE_H_
