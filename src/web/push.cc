// Copyright 2026 Timon Gentzsch

#include "include/web/push.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/fs.h"
#include "include/tools/files.h"
#include "include/web/protocol.h"
#ifdef UAGENT_WEB_PUSH
#include <openssl/rand.h>

#include "include/web/push_crypto.h"
#endif

namespace uagent::web {
std::string PushServiceOrigin(const std::string& endpoint) {
  if (endpoint.size() > 4096 ||
      endpoint.find_first_of("@#\\\r\n\t ") != std::string::npos) {
    return {};
  }
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(),
                                                          curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, endpoint.c_str(), 0) !=
                  CURLUE_OK) {
    return {};
  }
  auto part = [&](CURLUPart field) {
    char* value = nullptr;
    if (curl_url_get(url.get(), field, &value, 0) != CURLUE_OK) {
      return std::string();
    }
    std::string result(value);
    curl_free(value);
    return result;
  };
  if (part(CURLUPART_SCHEME) != "https" || !part(CURLUPART_USER).empty() ||
      !part(CURLUPART_PASSWORD).empty() || !part(CURLUPART_FRAGMENT).empty() ||
      (!part(CURLUPART_PORT).empty() && part(CURLUPART_PORT) != "443")) {
    return {};
  }
  std::string host = part(CURLUPART_HOST);
  if (host != "fcm.googleapis.com" &&
      host != "updates.push.services.mozilla.com" &&
      host != "web.push.apple.com") {
    return {};
  }
  return "https://" + host;
}

bool PublicPushAddress(const sockaddr* address) {
  if (!address) {
    return false;
  }
  if (address->sa_family == AF_INET) {
    const auto* value = reinterpret_cast<const sockaddr_in*>(address);
    if (ntohs(value->sin_port) != 443) {
      return false;
    }
    uint32_t ip = ntohl(value->sin_addr.s_addr);
    constexpr std::pair<uint32_t, unsigned> kExcluded[] = {
        {0x00000000, 8},  {0x0a000000, 8},  {0x64400000, 10}, {0x7f000000, 8},
        {0xa9fe0000, 16}, {0xac100000, 12}, {0xc0000000, 24}, {0xc0000200, 24},
        {0xc0a80000, 16}, {0xc6120000, 15}, {0xc6336400, 24}, {0xcb007100, 24},
        {0xe0000000, 3},
    };
    for (auto [network, bits] : kExcluded) {
      if ((ip & (UINT32_MAX << (32 - bits))) == network) {
        return false;
      }
    }
    return true;
  }
  if (address->sa_family == AF_INET6) {
    const auto* value = reinterpret_cast<const sockaddr_in6*>(address);
    const auto* bytes = value->sin6_addr.s6_addr;
    if (ntohs(value->sin6_port) != 443 || value->sin6_scope_id != 0 ||
        (bytes[0] & 0xe0) != 0x20) {
      return false;
    }
    if (bytes[0] == 0x20 && bytes[1] == 0x01 &&
        ((bytes[2] == 0x0d && bytes[3] == 0xb8) ||
         (bytes[2] == 0 && bytes[3] <= 0x2f))) {
      return false;
    }
    return true;
  }
  return false;
}

#ifdef UAGENT_WEB_PUSH
namespace {
curl_socket_t OpenSocket(void*, curlsocktype purpose, curl_sockaddr* address) {
  if (purpose != CURLSOCKTYPE_IPCXN || !PublicPushAddress(&address->addr)) {
    return CURL_SOCKET_BAD;
  }
  int type = address->socktype;
#ifdef SOCK_CLOEXEC
  type |= SOCK_CLOEXEC;
#endif
  int fd = socket(address->family, type, address->protocol);
  if (fd >= 0) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
  return fd;
}
}  // namespace
#endif

struct PushSender::Impl {
  std::string reason = "native Web Push is not compiled (UAGENT_WEB_PUSH=OFF)";
#ifdef UAGENT_WEB_PUSH
  struct Subscription {
    std::string device, endpoint, public_key, auth;
  };
  struct Attention {
    std::string id, session, device;
    std::chrono::steady_clock::time_point created;
  };
  std::string directory, contact;
  PushKey key;
  PushTransport transport;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::vector<Subscription> subscriptions;
  std::deque<Attention> queue;
  std::deque<std::string> seen;
  std::atomic<bool> stopping{false};
  std::thread sender;

  bool Persist() const {
    json value = json::array();
    for (const auto& item : subscriptions) {
      value.push_back({{"device", item.device},
                       {"endpoint", item.endpoint},
                       {"public_key", item.public_key},
                       {"auth", item.auth}});
    }
    return ToolWritePrivateFile(directory + "/push-subscriptions.json",
                                JsonDump(value))
        .Ok();
  }
  bool Valid(const Subscription& subscription) const {
    return OpaqueId(subscription.device) &&
           !PushServiceOrigin(subscription.endpoint).empty() &&
           Unbase64Url(subscription.auth).size() == 16 &&
           ImportPushKey(Unbase64Url(subscription.public_key)) != nullptr;
  }
  int64_t Deliver(const std::string& endpoint, const std::string& authorization,
                  const std::string& body) {
    if (transport) {
      return transport(endpoint, authorization, body);
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(
        curl_easy_init(), curl_easy_cleanup);
    if (!request) {
      return 0;
    }
    curl_slist* raw = nullptr;
    for (const std::string& header :
         {std::string("Content-Encoding: aes128gcm"),
          std::string("Content-Type: application/octet-stream"),
          std::string("TTL: 60"), std::string("Urgency: normal"),
          "Authorization: " + authorization}) {
      raw = curl_slist_append(raw, header.c_str());
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        raw, curl_slist_free_all);
    curl_easy_setopt(request.get(), CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(request.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(request.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(request.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(request.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(request.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(request.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(request.get(), CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(request.get(), CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(request.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(request.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(request.get(), CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(
        request.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
            -> int { return static_cast<Impl*>(opaque)->stopping ? 1 : 0; });
    curl_easy_setopt(request.get(), CURLOPT_OPENSOCKETFUNCTION, OpenSocket);
    size_t received = 0;
    curl_easy_setopt(request.get(), CURLOPT_WRITEDATA, &received);
    curl_easy_setopt(
        request.get(), CURLOPT_WRITEFUNCTION,
        +[](char*, size_t size, size_t count, void* opaque) -> size_t {
          auto& total = *static_cast<size_t*>(opaque);
          if (size > 65536 || count > 65536 || size * count > 65536 - total) {
            return 0;
          }
          total += size * count;
          return size * count;
        });
    if (curl_easy_perform(request.get()) != CURLE_OK) {
      return 0;
    }
    long status = 0;  // NOLINT: libcurl response-code ABI requires long.
    curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &status);
    return status;
  }
  void Run() {
    for (;;) {
      Attention attention;
      std::vector<Subscription> targets;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping) {
          return;
        }
        attention = std::move(queue.front());
        queue.pop_front();
        targets = subscriptions;
      }
      for (const auto& subscription : targets) {
        if (stopping) {
          return;
        }
        if (!attention.device.empty() &&
            attention.device != subscription.device) {
          continue;
        }
        if (std::chrono::steady_clock::now() - attention.created >
            std::chrono::seconds(60)) {
          break;
        }
        {
          std::lock_guard lock(mutex);
          if (std::none_of(subscriptions.begin(), subscriptions.end(),
                           [&](const Subscription& current) {
                             return current.device == subscription.device &&
                                    current.endpoint == subscription.endpoint;
                           })) {
            continue;
          }
        }
        std::string salt(16, '\0');
        PushKey ephemeral = GeneratePushKey();
        if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()),
                       static_cast<int>(salt.size())) != 1) {
          continue;
        }
        std::string body =
            EncryptPush(ephemeral.get(), Unbase64Url(subscription.public_key),
                        Unbase64Url(subscription.auth), salt,
                        JsonDump({{"id", attention.id},
                                  {"session_id", attention.session}}));
        std::string authorization = VapidAuthorization(
            key.get(), PushServiceOrigin(subscription.endpoint), contact,
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                    .count() +
                3600);
        if (body.empty() || authorization.empty()) {
          continue;
        }
        for (int attempt = 0; attempt < 2 && !stopping; ++attempt) {
          int64_t status = Deliver(subscription.endpoint, authorization, body);
          if (status == 404 || status == 410) {
            std::lock_guard lock(mutex);
            std::erase_if(subscriptions, [&](const Subscription& current) {
              return current.device == subscription.device &&
                     current.endpoint == subscription.endpoint;
            });
            Persist();
            break;
          }
          if (status != 0 && status != 429 && status < 500) {
            break;
          }
          std::unique_lock lock(mutex);
          if (changed.wait_for(lock, std::chrono::seconds(1),
                               [&] { return stopping.load(); })) {
            return;
          }
        }
      }
    }
  }
#endif
};

PushSender::PushSender(const std::string& directory, const std::string& contact,
                       const PushTransport& transport)
    : impl_(std::make_unique<Impl>()) {
#ifdef UAGENT_WEB_PUSH
  impl_->directory = directory;
  impl_->contact = contact;
  impl_->transport = transport;
  bool valid_contact = contact.size() <= 200 &&
                       contact.find_first_of("\r\n\t ") == std::string::npos &&
                       ((contact.starts_with("mailto:") &&
                         contact.find('@') != std::string::npos) ||
                        contact.starts_with("https://"));
  if (!valid_contact) {
    impl_->reason =
        "set UAGENT_WEB_PUSH_CONTACT to a mailto or HTTPS contact to enable "
        "native Web Push";
    return;
  }
  const auto* curl = curl_version_info(CURLVERSION_NOW);
  if (!curl || !(curl->features & CURL_VERSION_ASYNCHDNS)) {
    impl_->reason = "Web Push requires libcurl with asynchronous DNS";
    return;
  }
  impl_->key = LoadPushKey(directory + "/vapid.pem");
  if (!impl_->key) {
    impl_->reason = "cannot load the private VAPID key";
    return;
  }
  std::string bytes, error;
  if (ReadRegularFile(directory + "/push-subscriptions.json",
                      size_t{128} * 1024, bytes, error)) {
    json entries = json::parse(bytes, nullptr, false);
    if (entries.is_array() && entries.size() <= 16) {
      for (const json& entry : entries) {
        Impl::Subscription subscription{
            JsonValue(entry, "device", ""), JsonValue(entry, "endpoint", ""),
            JsonValue(entry, "public_key", ""), JsonValue(entry, "auth", "")};
        if (impl_->Valid(subscription)) {
          impl_->subscriptions.push_back(std::move(subscription));
        }
      }
    }
  }
  impl_->reason.clear();
  impl_->sender = std::thread([this] { impl_->Run(); });
#else
  (void)directory;
  (void)contact;
  (void)transport;
#endif
}
PushSender::~PushSender() {
#ifdef UAGENT_WEB_PUSH
  impl_->stopping = true;
  impl_->changed.notify_all();
  if (impl_->sender.joinable()) {
    impl_->sender.join();
  }
#endif
}
json PushSender::Capabilities(const std::string& device) const {
  json result = {{"push", impl_->reason.empty()},
                 {"push_reason", impl_->reason},
                 {"subscribed", false}};
#ifdef UAGENT_WEB_PUSH
  std::lock_guard lock(impl_->mutex);
  if (impl_->key) {
    result["vapid_public_key"] = Base64Url(PushPublicKey(impl_->key.get()));
  }
  result["subscribed"] =
      std::any_of(impl_->subscriptions.begin(), impl_->subscriptions.end(),
                  [&](const auto& item) { return item.device == device; });
#else
  (void)device;
#endif
  return result;
}
bool PushSender::Subscribe(const std::string& device, const json& subscription,
                           std::string& error) {
  error = impl_->reason;
  if (!error.empty()) {
    return false;
  }
#ifdef UAGENT_WEB_PUSH
  json keys = JsonValue(subscription, "keys", json::object());
  Impl::Subscription item{device, JsonValue(subscription, "endpoint", ""),
                          JsonValue(keys, "p256dh", ""),
                          JsonValue(keys, "auth", "")};
  if (!impl_->Valid(item)) {
    error = "invalid subscription or unsupported HTTPS push service";
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  auto previous = impl_->subscriptions;
  std::erase_if(impl_->subscriptions,
                [&](const auto& current) { return current.device == device; });
  if (impl_->subscriptions.size() >= 16) {
    impl_->subscriptions = std::move(previous);
    error = "push device limit reached";
    return false;
  }
  impl_->subscriptions.push_back(std::move(item));
  if (!impl_->Persist()) {
    impl_->subscriptions = std::move(previous);
    error = "cannot save push subscription";
    return false;
  }
  return true;
#else
  (void)device;
  (void)subscription;
  return false;
#endif
}
bool PushSender::Revoke(const std::string& device) {
#ifdef UAGENT_WEB_PUSH
  std::lock_guard lock(impl_->mutex);
  auto previous = impl_->subscriptions;
  std::erase_if(impl_->subscriptions,
                [&](const auto& item) { return item.device == device; });
  if (!impl_->Persist()) {
    impl_->subscriptions = std::move(previous);
    return false;
  }
#else
  (void)device;
#endif
  return true;
}
bool PushSender::Notify(const std::string& event, const std::string& session,
                        const std::string& device) {
#ifdef UAGENT_WEB_PUSH
  if (!impl_->reason.empty() || event.size() > 150 || !OpaqueId(session)) {
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->queue.size() >= 64 ||
      std::find(impl_->seen.begin(), impl_->seen.end(), event + ":" + device) !=
          impl_->seen.end()) {
    return false;
  }
  impl_->seen.push_back(event + ":" + device);
  if (impl_->seen.size() > 256) {
    impl_->seen.pop_front();
  }
  impl_->queue.push_back(
      {event, session, device, std::chrono::steady_clock::now()});
  impl_->changed.notify_one();
  return true;
#else
  (void)event;
  (void)session;
  (void)device;
  return false;
#endif
}
}  // namespace uagent::web
