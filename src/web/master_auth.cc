// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"
#include "include/core/thread_annotations.h"
#include "include/core/time.h"
#include "include/tools/files.h"
#include "include/web/protocol.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {

namespace {
constexpr size_t kAuthenticationRequestBytes = KiB(4);
constexpr size_t kDeviceStoreBytes = KiB(64);
constexpr size_t kDeviceNameChars = 80;
constexpr int kAuthenticationAttempts = 12;
constexpr auto kPairingLifetime = std::chrono::minutes(5);
constexpr auto kAuthenticationWindow = std::chrono::minutes(1);
constexpr auto kDeviceLifetime = std::chrono::hours(24 * 30);
}  // namespace

bool EqualSecret(std::string_view a, std::string_view b) {
  if (a.size() != b.size() || a.empty()) {
    return false;
  }
  unsigned difference = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    difference |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return difference == 0;
}

std::string Master::Pair() {
  pair_ = RandomToken(12);
  pair_deadline_ = Clock::now() + kPairingLifetime;
  return pair_;
}
json Master::PublicDevices() const {
  json out = json::array();
  for (const Device& device : devices_) {
    out.push_back({{"id", device.id}, {"name", device.name}});
  }
  return out;
}
void Master::LoadDevices() {
  const json value =
      ReadJsonFile(directory_ + "/devices.json", kDeviceStoreBytes);
  if (!value.is_array() || value.size() > kWebDeviceLimit) {
    return;
  }
  std::lock_guard lock(mutex_);
  for (const json& item : value) {
    Device device{JsonValue(item, "id", ""), JsonValue(item, "token", ""),
                  JsonValue(item, "name", "Device"),
                  JsonValue(item, "expires", int64_t{0})};
    if (OpaqueId(device.id) && OpaqueId(device.token) && device.Active()) {
      devices_.push_back(std::move(device));
    }
  }
}
bool Master::SaveDevices() const {
  json out = json::array();
  for (const Device& device : devices_) {
    out.push_back({{"id", device.id},
                   {"token", device.token},
                   {"name", device.name},
                   {"expires", device.expires}});
  }
  std::string error;
  return WritePrivateFile(directory_ + "/devices.json", JsonDump(out), error);
}
std::string Master::DeviceId(const Request& request) const {
  std::string cookies = request.get_header_value("Cookie");
  // Only this one cookie; reject duplicate credentials instead of choosing.
  std::string token;
  for (const std::string& entry : SplitPathList(cookies, ';')) {
    std::string part = Trim(entry);
    if (!part.starts_with("uagent_device=")) {
      continue;
    }
    if (!token.empty()) {
      return {};
    }
    token = part.substr(14);
  }
  for (const Device& device : devices_) {
    if (device.Active() && EqualSecret(token, device.token)) {
      return device.id;
    }
  }
  return {};
}
void Master::Authenticate(const Request& request, Response& response) {
  if (request.body.size() > kAuthenticationRequestBytes) {
    Error(response, "authentication request too large", 413);
    return;
  }
  json input = json::parse(request.body, nullptr, false);
  std::lock_guard lock(mutex_);
  auto now = Clock::now();
  if (now - auth_window_ > kAuthenticationWindow) {
    auth_window_ = now;
    auth_attempts_ = 0;
  }
  if (++auth_attempts_ > kAuthenticationAttempts) {
    Error(response, "pairing rate limit; retry in a minute", 429);
    return;
  }
  if (now > pair_deadline_ ||
      !EqualSecret(JsonValue(input, "code", ""), pair_)) {
    Error(response, "invalid or expired pairing code", 403);
    return;
  }
  std::erase_if(devices_,
                [](const Device& device) { return !device.Active(); });
  if (devices_.size() >= kWebDeviceLimit) {
    Error(response, "device limit reached; revoke a device first", 409);
    return;
  }
  Device device{
      RandomToken(16), RandomToken(32),
      Utf8Prefix(JsonValue(input, "name", "Browser"), kDeviceNameChars),
      NowMillis() +
          std::chrono::duration_cast<std::chrono::milliseconds>(kDeviceLifetime)
              .count()};
  if (device.id.empty() || device.token.empty()) {
    Error(response, "randomness unavailable", 500);
    return;
  }
  devices_.push_back(device);
  if (!SaveDevices()) {
    devices_.pop_back();
    Error(response, "cannot persist device", 500);
    return;
  }
  pair_.clear();
  const std::string* expected_origin = ExpectedOrigin(request);
  response.set_header(
      "Set-Cookie",
      "uagent_device=" + device.token +
          "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
          std::to_string(
              std::chrono::duration_cast<std::chrono::seconds>(kDeviceLifetime)
                  .count()) +
          (expected_origin && expected_origin->starts_with("https://")
               ? "; Secure"
               : ""));
  Reply(response, {{"v", kProtocol}, {"device", device.id}});
}
}  // namespace uagent::web::master
