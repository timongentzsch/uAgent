// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_REMOTE_HOSTS_H_
#define UAGENT_INCLUDE_WEB_REMOTE_HOSTS_H_
#include <memory>
#include <optional>
#include <string>

#include "include/core/json.h"
namespace uagent::web {
// Return the manifest only after every payload matches this build and its hash.
std::optional<json> CheckRemoteBundle(const std::string& directory,
                                      std::string& error);

// Serving-host owned SSH transports. Reads never initiate a connection.
class RemoteHosts {
 public:
  RemoteHosts(const std::string& executable, const std::string& bundle);
  ~RemoteHosts();
  json List() const;
  json Ping(const std::string& id) const;
  json Change(const json& action, std::string& error);
  json Target(const std::string& id, const std::string& cwd,
              const std::string& runtime) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace uagent::web
#endif
