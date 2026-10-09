// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PRIVATE_STORE_H_
#define UAGENT_INCLUDE_CORE_PRIVATE_STORE_H_

#include <cstddef>
#include <string>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent {

// A private JSON file as it parses, or a discarded value when it is missing,
// is no regular file, exceeds `byte_limit` or is not JSON: for a reader that
// treats all of those as "nothing there".
json ReadJsonFile(const std::string& path, size_t byte_limit);

// A small owner-only JSON document guarded across worker processes. Callers
// validate their own schema; this class owns only locking, parsing and atomic
// replacement.
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
