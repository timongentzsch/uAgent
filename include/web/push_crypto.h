// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_PUSH_CRYPTO_H_
#define UAGENT_INCLUDE_WEB_PUSH_CRYPTO_H_

#include <openssl/evp.h>

#include <memory>
#include <string>
#include <string_view>

namespace uagent::web {
struct PushKeyDeleter {
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
using PushKey = std::unique_ptr<EVP_PKEY, PushKeyDeleter>;
std::string Base64Url(std::string_view bytes);
std::string Unbase64Url(std::string_view encoded);
PushKey GeneratePushKey();
PushKey ImportPushKey(std::string_view public_bytes,
                      std::string_view private_bytes = {});
std::string PushPublicKey(EVP_PKEY* key);
PushKey LoadPushKey(const std::string& path);
// Salt is exactly 16 bytes. The caller generates a fresh ephemeral sender key
// and salt for every encryption; explicit inputs allow RFC fixture testing.
std::string EncryptPush(EVP_PKEY* ephemeral, std::string_view receiver_public,
                        std::string_view auth, std::string_view salt,
                        std::string_view plaintext);
std::string VapidAuthorization(EVP_PKEY* key, const std::string& audience,
                               const std::string& contact, int64_t expires);
}  // namespace uagent::web
#endif  // UAGENT_INCLUDE_WEB_PUSH_CRYPTO_H_
