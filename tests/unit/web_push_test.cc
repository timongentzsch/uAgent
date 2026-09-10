// Copyright 2026 Timon Gentzsch
#include <arpa/inet.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "include/core/fs.h"
#include "include/web/push.h"
#include "include/web/push_crypto.h"

#define REQUIRE(value)                                                \
  do {                                                                \
    if (!(value)) {                                                   \
      std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #value); \
      return 1;                                                       \
    }                                                                 \
  } while (false)

namespace uagent::web {
int TestPush() {
  const std::string as_public = Unbase64Url(
      "BP4z9KsN6nGRTbVYI_"
      "c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8");
  const std::string as_private =
      Unbase64Url("yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw");
  const std::string ua_public = Unbase64Url(
      "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-"
      "AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4");
  const std::string salt = Unbase64Url("DGv6ra1nlYgDCS1FRnbzlw");
  const std::string auth = Unbase64Url("BTBZMqHH6r4Tts7J_aSIgg");
  PushKey key = ImportPushKey(as_public, as_private);
  REQUIRE(key != nullptr);
  // RFC 8291 section 5 / appendix A, exact header and ciphertext.
  std::string body = EncryptPush(key.get(), ua_public, auth, salt,
                                 "When I grow up, I want to be a watermelon");
  REQUIRE(body.size() == 144);
  REQUIRE(
      Base64Url(body.substr(0, 86)) ==
      "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_"
      "c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8");
  REQUIRE(Base64Url(body.substr(86)) ==
          "8pfeW0KbunFT06SuDKoJH9Ql87S1QUrdirN6GcG7sFz1y1sqLgVi1VhjVkHsUoEsbI_"
          "0LpXMuGvnzQ");
  REQUIRE(EncryptPush(key.get(), "invalid", auth, salt, "test").empty());
  REQUIRE(Unbase64Url("%%%invalid").empty());
  REQUIRE(PushServiceOrigin("https://fcm.googleapis.com/send/test") ==
          "https://fcm.googleapis.com");
  for (const char* endpoint :
       {"http://fcm.googleapis.com/send", "https://fcm.googleapis.com:444/send",
        "https://user@fcm.googleapis.com/send", "https://127.0.0.1/send",
        "https://fcm.googleapis.com.evil.example/send"}) {
    REQUIRE(PushServiceOrigin(endpoint).empty());
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(443);
  for (const char* ip :
       {"127.0.0.1", "10.0.0.1", "169.254.169.254", "172.16.0.1", "192.168.1.1",
        "100.64.0.1", "224.0.0.1"}) {
    inet_pton(AF_INET, ip, &address.sin_addr);
    REQUIRE(!PublicPushAddress(reinterpret_cast<sockaddr*>(&address)));
  }
  inet_pton(AF_INET, "142.250.185.42", &address.sin_addr);
  REQUIRE(PublicPushAddress(reinterpret_cast<sockaddr*>(&address)));
  std::string authorization =
      VapidAuthorization(key.get(), "https://fcm.googleapis.com",
                         "mailto:test@example.com", 1800000000);
  REQUIRE(authorization.starts_with("vapid t="));
  size_t start = 8, end = authorization.find(", k=");
  std::string token = authorization.substr(start, end - start);
  size_t last = token.rfind('.');
  std::string raw_signature = Unbase64Url(token.substr(last + 1));
  REQUIRE(raw_signature.size() == 64);
  ECDSA_SIG* signature = ECDSA_SIG_new();
  REQUIRE(signature != nullptr);
  REQUIRE(
      ECDSA_SIG_set0(
          signature,
          BN_bin2bn(
              reinterpret_cast<const unsigned char*>(raw_signature.data()), 32,
              nullptr),
          BN_bin2bn(
              reinterpret_cast<const unsigned char*>(raw_signature.data()) + 32,
              32, nullptr)) == 1);
  unsigned char* der = nullptr;
  int der_size = i2d_ECDSA_SIG(signature, &der);
  EVP_MD_CTX* verify = EVP_MD_CTX_new();
  REQUIRE(verify && der_size > 0 &&
          EVP_DigestVerifyInit(verify, nullptr, EVP_sha256(), nullptr,
                               key.get()) == 1);
  REQUIRE(EVP_DigestVerify(verify, der, static_cast<size_t>(der_size),
                           reinterpret_cast<const unsigned char*>(token.data()),
                           last) == 1);
  EVP_MD_CTX_free(verify);
  OPENSSL_free(der);
  ECDSA_SIG_free(signature);
  char temporary[] = "/tmp/uagent-push-test-XXXXXX";
  REQUIRE(mkdtemp(temporary) != nullptr);
  std::atomic<int> deliveries{0};
  std::atomic<bool> private_payload{true};
  {
    PushSender sender(
        temporary, "mailto:test@example.com",
        [&](const std::string& endpoint, const std::string& header,
            const std::string& payload) {
          if (!endpoint.starts_with("https://fcm.googleapis.com/") ||
              !header.starts_with("vapid t=") || payload.size() < 102 ||
              payload.find("prompt") != std::string::npos) {
            private_payload = false;
          }
          ++deliveries;
          return 410L;
        });
    std::string device(32, 'a'), error;
    REQUIRE(JsonValue(sender.Capabilities(device), "push", false));
    REQUIRE(sender.Subscribe(
        device,
        {{"endpoint", "https://fcm.googleapis.com/send/test"},
         {"keys",
          {{"p256dh", Base64Url(ua_public)}, {"auth", Base64Url(auth)}}}},
        error));
    REQUIRE(sender.Notify("test:1", std::string(32, 'b'), device));
    REQUIRE(!sender.Notify("test:1", std::string(32, 'b'), device));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (JsonValue(sender.Capabilities(device), "subscribed", false) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(deliveries == 1 && private_payload);
    REQUIRE(!JsonValue(sender.Capabilities(device), "subscribed", true));
  }
  std::filesystem::remove_all(temporary);
  std::puts(
      "Web Push: RFC 8291 vector, VAPID verification, SSRF policy, "
      "deduplication and expiry passed");
  return 0;
}

}  // namespace uagent::web

int main() { return uagent::web::TestPush(); }
