// Copyright 2026 Timon Gentzsch

#include "include/web/push_crypto.h"

#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>

#include "include/core/fs.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::web {
namespace {
template <typename T, auto Destroy>
struct Cleanup {
  void operator()(T* value) const { Destroy(value); }
};
template <typename T, auto Destroy>
using Owned = std::unique_ptr<T, Cleanup<T, Destroy>>;
using KeyContext = Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;

std::string Hkdf(std::string_view key, std::string_view salt,
                 std::string_view info, size_t length) {
  Owned<EVP_KDF, EVP_KDF_free> algorithm(
      EVP_KDF_fetch(nullptr, "HKDF", nullptr));
  Owned<EVP_KDF_CTX, EVP_KDF_CTX_free> context(
      algorithm ? EVP_KDF_CTX_new(algorithm.get()) : nullptr);
  if (!context) {
    return {};
  }
  char digest[] = "SHA256";
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_KEY, const_cast<char*>(key.data()), key.size()),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_SALT, const_cast<char*>(salt.data()), salt.size()),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_INFO, const_cast<char*>(info.data()), info.size()),
      OSSL_PARAM_construct_end(),
  };
  std::string result(length, '\0');
  if (EVP_KDF_derive(context.get(),
                     reinterpret_cast<unsigned char*>(result.data()),
                     result.size(), parameters) != 1) {
    return {};
  }
  return result;
}
}  // namespace

std::string Base64Url(std::string_view bytes) {
  if (bytes.size() > 8192) {
    return {};
  }
  std::string result((bytes.size() + 2) / 3 * 4 + 1, '\0');
  int length =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                      reinterpret_cast<const unsigned char*>(bytes.data()),
                      static_cast<int>(bytes.size()));
  if (length < 0) {
    return {};
  }
  result.resize(static_cast<size_t>(length));
  std::replace(result.begin(), result.end(), '+', '-');
  std::replace(result.begin(), result.end(), '/', '_');
  while (!result.empty() && result.back() == '=') {
    result.pop_back();
  }
  return result;
}

std::string Unbase64Url(std::string_view encoded) {
  if (encoded.size() > 4096 ||
      encoded.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") !=
          std::string_view::npos) {
    return {};
  }
  std::string input(encoded), result;
  std::replace(input.begin(), input.end(), '-', '+');
  std::replace(input.begin(), input.end(), '_', '/');
  while (input.size() % 4) {
    input += '=';
  }
  return Base64Decode(input, result, 3072) ? result : std::string();
}

PushKey GeneratePushKey() {
  KeyContext context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
  char group[] = "prime256v1";
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
      OSSL_PARAM_construct_end()};
  EVP_PKEY* key = nullptr;
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
      EVP_PKEY_CTX_set_params(context.get(), parameters) != 1 ||
      EVP_PKEY_generate(context.get(), &key) != 1) {
    return {};
  }
  return PushKey(key);
}

PushKey ImportPushKey(std::string_view public_bytes,
                      std::string_view private_bytes) {
  if (public_bytes.size() != 65 || public_bytes.front() != '\x04' ||
      (!private_bytes.empty() && private_bytes.size() != 32)) {
    return {};
  }
  KeyContext context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
  Owned<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free> builder(OSSL_PARAM_BLD_new());
  if (!context || !builder ||
      OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME,
                                      "prime256v1", 0) != 1 ||
      OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY,
                                       public_bytes.data(),
                                       public_bytes.size()) != 1) {
    return {};
  }
  Owned<BIGNUM, BN_clear_free> scalar;
  if (!private_bytes.empty()) {
    scalar.reset(
        BN_bin2bn(reinterpret_cast<const unsigned char*>(private_bytes.data()),
                  static_cast<int>(private_bytes.size()), nullptr));
    if (!scalar ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PRIV_KEY,
                               scalar.get()) != 1) {
      return {};
    }
  }
  Owned<OSSL_PARAM, OSSL_PARAM_free> parameters(
      OSSL_PARAM_BLD_to_param(builder.get()));
  EVP_PKEY* raw = nullptr;
  if (!parameters || EVP_PKEY_fromdata_init(context.get()) != 1 ||
      EVP_PKEY_fromdata(
          context.get(), &raw,
          private_bytes.empty() ? EVP_PKEY_PUBLIC_KEY : EVP_PKEY_KEYPAIR,
          parameters.get()) != 1) {
    return {};
  }
  PushKey key(raw);
  KeyContext check(EVP_PKEY_CTX_new(key.get(), nullptr));
  if (!check || EVP_PKEY_public_check(check.get()) != 1 ||
      (!private_bytes.empty() && EVP_PKEY_pairwise_check(check.get()) != 1)) {
    return {};
  }
  return key;
}

std::string PushPublicKey(EVP_PKEY* key) {
  std::string result(65, '\0');
  size_t size = 0;
  if (!key ||
      EVP_PKEY_get_octet_string_param(
          key, OSSL_PKEY_PARAM_PUB_KEY,
          reinterpret_cast<unsigned char*>(result.data()), result.size(),
          &size) != 1 ||
      size != result.size()) {
    return {};
  }
  return result;
}

PushKey LoadPushKey(const std::string& path) {
  std::string contents, error;
  if (PathExists(path)) {
    if (!ReadRegularFile(path, 8192, contents, error)) {
      return {};
    }
    Owned<BIO, BIO_free> input(
        BIO_new_mem_buf(contents.data(), static_cast<int>(contents.size())));
    PushKey key(
        input ? PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr)
              : nullptr);
    if (!key || PushPublicKey(key.get()).size() != 65) {
      return {};
    }
    return key;
  }
  PushKey key = GeneratePushKey();
  Owned<BIO, BIO_free> output(BIO_new(BIO_s_mem()));
  if (!key || !output ||
      PEM_write_bio_PrivateKey(output.get(), key.get(), nullptr, nullptr, 0,
                               nullptr, nullptr) != 1) {
    return {};
  }
  char* bytes = nullptr;
  auto count = BIO_get_mem_data(output.get(), &bytes);
  if (count <= 0 || !ToolWritePrivateFile(
                         path, std::string(bytes, static_cast<size_t>(count)))
                         .Ok()) {
    return {};
  }
  return key;
}

std::string EncryptPush(EVP_PKEY* ephemeral, std::string_view receiver_public,
                        std::string_view auth, std::string_view salt,
                        std::string_view plaintext) {
  if (!ephemeral || auth.size() != 16 || salt.size() != 16 ||
      plaintext.size() > 2048) {
    return {};
  }
  PushKey receiver = ImportPushKey(receiver_public);
  KeyContext exchange(EVP_PKEY_CTX_new(ephemeral, nullptr));
  std::string shared(32, '\0');
  size_t size = shared.size();
  if (!receiver || !exchange || EVP_PKEY_derive_init(exchange.get()) != 1 ||
      EVP_PKEY_derive_set_peer(exchange.get(), receiver.get()) != 1 ||
      EVP_PKEY_derive(exchange.get(),
                      reinterpret_cast<unsigned char*>(shared.data()),
                      &size) != 1 ||
      size != shared.size()) {
    return {};
  }
  std::string public_bytes = PushPublicKey(ephemeral);
  if (public_bytes.empty()) {
    return {};
  }
  std::string info("WebPush: info\0", 14);
  info += receiver_public;
  info += public_bytes;
  std::string input = Hkdf(shared, auth, info, 32);
  std::string key =
      Hkdf(input, salt,
           std::string_view("Content-Encoding: aes128gcm",
                            sizeof("Content-Encoding: aes128gcm")),
           16);
  std::string nonce = Hkdf(input, salt,
                           std::string_view("Content-Encoding: nonce",
                                            sizeof("Content-Encoding: nonce")),
                           12);
  if (input.empty() || key.empty() || nonce.empty()) {
    return {};
  }
  Owned<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> cipher(EVP_CIPHER_CTX_new());
  std::string padded(plaintext);
  padded += '\x02';
  std::string encrypted(padded.size() + 16, '\0');
  int count = 0, tail = 0;
  if (!cipher ||
      EVP_EncryptInit_ex(
          cipher.get(), EVP_aes_128_gcm(), nullptr,
          reinterpret_cast<const unsigned char*>(key.data()),
          reinterpret_cast<const unsigned char*>(nonce.data())) != 1 ||
      EVP_EncryptUpdate(
          cipher.get(), reinterpret_cast<unsigned char*>(encrypted.data()),
          &count, reinterpret_cast<const unsigned char*>(padded.data()),
          static_cast<int>(padded.size())) != 1 ||
      EVP_EncryptFinal_ex(
          cipher.get(),
          reinterpret_cast<unsigned char*>(encrypted.data()) + count,
          &tail) != 1 ||
      count < 0 || tail < 0 ||
      static_cast<size_t>(count) + static_cast<size_t>(tail) != padded.size() ||
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16,
                          encrypted.data() + padded.size()) != 1) {
    return {};
  }
  std::string header(salt);
  header.append("\x00\x00\x10\x00\x41", 5);
  header += public_bytes;
  return header + encrypted;
}

std::string VapidAuthorization(EVP_PKEY* key, const std::string& audience,
                               const std::string& contact, int64_t expires) {
  std::string token =
      Base64Url("{\"typ\":\"JWT\",\"alg\":\"ES256\"}") + "." +
      Base64Url(
          JsonDump({{"aud", audience}, {"exp", expires}, {"sub", contact}}));
  Owned<EVP_MD_CTX, EVP_MD_CTX_free> signer(EVP_MD_CTX_new());
  size_t count = 0;
  if (!key || !signer ||
      EVP_DigestSignInit(signer.get(), nullptr, EVP_sha256(), nullptr, key) !=
          1 ||
      EVP_DigestSign(signer.get(), nullptr, &count,
                     reinterpret_cast<const unsigned char*>(token.data()),
                     token.size()) != 1 ||
      count > 128) {
    return {};
  }
  std::string der(count, '\0');
  if (EVP_DigestSign(signer.get(), reinterpret_cast<unsigned char*>(der.data()),
                     &count,
                     reinterpret_cast<const unsigned char*>(token.data()),
                     token.size()) != 1) {
    return {};
  }
  const unsigned char* bytes =
      reinterpret_cast<const unsigned char*>(der.data());
  Owned<ECDSA_SIG, ECDSA_SIG_free> signature(d2i_ECDSA_SIG(
      nullptr, &bytes,
      static_cast<long>(count)));  // NOLINT: OpenSSL ABI requires long.
  std::string raw(64, '\0');
  if (!signature ||
      BN_bn2binpad(ECDSA_SIG_get0_r(signature.get()),
                   reinterpret_cast<unsigned char*>(raw.data()), 32) != 32 ||
      BN_bn2binpad(ECDSA_SIG_get0_s(signature.get()),
                   reinterpret_cast<unsigned char*>(raw.data()) + 32,
                   32) != 32) {
    return {};
  }
  return "vapid t=" + token + "." + Base64Url(raw) +
         ", k=" + Base64Url(PushPublicKey(key));
}
}  // namespace uagent::web
