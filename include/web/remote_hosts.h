// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_REMOTE_HOSTS_H_
#define UAGENT_INCLUDE_WEB_REMOTE_HOSTS_H_
#include <memory>
#include <optional>
#include <string>

#include "include/core/json.h"
#include "include/web/remote_proxy.h"
namespace uagent::web {
bool CheckRemoteBundle(const std::string& directory, std::string& error);

// Serving-host owned SSH transports. Reads never initiate a connection.
class RemoteHosts {
 public:
  explicit RemoteHosts(std::string executable);
  ~RemoteHosts();
  json List() const;
  json Change(const json& action, std::string& error);
  std::optional<RemoteEndpoint> Endpoint(const std::string& id) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace uagent::web
#endif
