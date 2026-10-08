// Copyright 2026 Timon Gentzsch
// The web host without Web Push (UAGENT_WEB_PUSH=OFF): a sender that reports
// why it cannot push and accepts no subscription.

#include <memory>
#include <string>

#include "include/web/push.h"

namespace uagent::web {

struct PushSender::Impl {
  std::string reason = "native Web Push is not compiled (UAGENT_WEB_PUSH=OFF)";
};

PushSender::PushSender(const std::string& directory, const std::string& contact,
                       const PushTransport& transport)
    : impl_(std::make_unique<Impl>()) {
  (void)directory;
  (void)contact;
  (void)transport;
}
PushSender::~PushSender() {}
json PushSender::Capabilities(const std::string& device) const {
  json result = {{"push", impl_->reason.empty()},
                 {"push_reason", impl_->reason},
                 {"subscribed", false}};
  (void)device;
  return result;
}
bool PushSender::Subscribe(const std::string& device, const json& subscription,
                           std::string& error) {
  error = impl_->reason;
  if (!error.empty()) {
    return false;
  }
  (void)device;
  (void)subscription;
  return false;
}
bool PushSender::Revoke(const std::string& device) {
  (void)device;
  return true;
}
bool PushSender::Notify(const std::string& event, const std::string& session,
                        const std::string& device) {
  (void)event;
  (void)session;
  (void)device;
  return false;
}
}  // namespace uagent::web
