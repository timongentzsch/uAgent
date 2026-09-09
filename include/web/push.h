// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_PUSH_H_
#define UAGENT_INCLUDE_WEB_PUSH_H_
#include <sys/socket.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "include/core/json.h"
namespace uagent::web {
// Fixed service allowlist plus connection-time address validation. Exported for
// hermetic security tests; no user-configurable localhost/redirect exception.
std::string PushServiceOrigin(const std::string& endpoint);
bool PublicPushAddress(const sockaddr* address);
using PushTransport = std::function<int64_t(
    const std::string&, const std::string&, const std::string&)>;
class PushSender {
 public:
  PushSender(const std::string& directory, const std::string& contact,
             PushTransport transport = {});
  ~PushSender();
  PushSender(const PushSender&) = delete;
  PushSender& operator=(const PushSender&) = delete;
  json Capabilities(const std::string& device) const;
  bool Subscribe(const std::string& device, const json& subscription,
                 std::string& error);
  bool Revoke(const std::string& device);
  bool Notify(const std::string& event, const std::string& session,
              const std::string& device = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace uagent::web
#endif  // UAGENT_INCLUDE_WEB_PUSH_H_
