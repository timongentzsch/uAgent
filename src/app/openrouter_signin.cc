// Copyright 2026 Timon Gentzsch

#include "include/app/openrouter_signin.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include "include/api/client.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
namespace {

std::array<uint8_t, 32> Sha256(std::string_view text) {
  static constexpr uint32_t kRound[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string padded(text);
  padded.push_back('\x80');
  while (padded.size() % 64 != 56) padded.push_back('\0');
  const uint64_t bits = static_cast<uint64_t>(text.size()) * 8;
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<char>(bits >> shift));
  }
  auto rotate = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t block = 0; block < padded.size(); block += 64) {
    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
      const auto* p =
          reinterpret_cast<const uint8_t*>(padded.data()) + block + i * 4;
      w[i] = static_cast<uint32_t>(p[0]) << 24 |
             static_cast<uint32_t>(p[1]) << 16 |
             static_cast<uint32_t>(p[2]) << 8 | p[3];
    }
    for (size_t i = 16; i < 64; ++i) {
      const uint32_t s0 =
          rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 =
          rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    for (size_t i = 0; i < 8; ++i) v[i] = h[i];
    for (size_t i = 0; i < 64; ++i) {
      const uint32_t s1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25);
      const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const uint32_t t1 = v[7] + s1 + ch + kRound[i] + w[i];
      const uint32_t s0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22);
      const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      for (size_t j = 7; j > 0; --j) v[j] = v[j - 1];
      v[4] += t1;
      v[0] = t1 + s0 + maj;
    }
    for (size_t i = 0; i < 8; ++i) h[i] += v[i];
  }
  std::array<uint8_t, 32> digest{};
  for (size_t i = 0; i < 32; ++i) {
    digest[i] = static_cast<uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
  }
  return digest;
}

// OpenRouter's site, where the API is served under /api/v1.
std::string Site(std::string base_url) {
  constexpr std::string_view kApi = "/api/v1";
  if (base_url.ends_with(kApi)) base_url.resize(base_url.size() - kApi.size());
  return base_url;
}

}  // namespace

std::string PkceChallenge(std::string_view verifier) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const auto digest = Sha256(verifier);
  std::string out;
  for (size_t i = 0; i < digest.size(); i += 3) {
    const size_t left = digest.size() - i;
    const uint32_t group =
        static_cast<uint32_t>(digest[i]) << 16 |
        (left > 1 ? static_cast<uint32_t>(digest[i + 1]) << 8 : 0u) |
        (left > 2 ? static_cast<uint32_t>(digest[i + 2]) : 0u);
    out.push_back(kAlphabet[group >> 18]);
    out.push_back(kAlphabet[group >> 12 & 63]);
    if (left > 1) out.push_back(kAlphabet[group >> 6 & 63]);
    if (left > 2) out.push_back(kAlphabet[group & 63]);
  }
  return out;
}

SignIn BeginSignIn(const std::string& base_url, const std::string& callback) {
  SignIn start{
      .url = "", .verifier = RandomToken(32), .state = RandomToken(16)};
  start.url = Site(base_url) +
              "/auth?code_challenge=" + PkceChallenge(start.verifier) +
              "&code_challenge_method=S256&key_label=uAgent";
  if (!callback.empty()) {
    start.url +=
        "&state=" + start.state + "&callback_url=" + PercentEncode(callback);
  }
  return start;
}

std::string RedeemSignIn(const std::string& base_url, const std::string& code,
                         const std::string& verifier, std::string& error) {
  Api api;
  api.base_url = base_url;
  api.wire_api = WireApi::kChatCompletions;
  DeclareHost(api);
  const JsonResponse response = api.Post("/auth/keys",
                                         {{"code", code},
                                          {"code_verifier", verifier},
                                          {"code_challenge_method", "S256"}},
                                         20);
  std::string key;
  if (response.body.is_object()) key = JsonValue(response.body, "key", "");
  if (key.empty()) {
    error = response.http_status == 0
                ? response.error
                : "OpenRouter did not accept the code (" +
                      std::to_string(response.http_status) +
                      "); it works once, for ten minutes";
  }
  return key;
}

int ListenForSignIn(std::string& callback) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t size = sizeof(address);
  if (listener < 0 ||
      bind(listener, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
      listen(listener, 4) != 0 ||
      getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) !=
          0) {
    if (listener >= 0) close(listener);
    return -1;
  }
  fcntl(listener, F_SETFD, FD_CLOEXEC);
  callback = "http://localhost:" + std::to_string(ntohs(address.sin_port)) +
             "/callback";
  return listener;
}

std::string AcceptSignIn(int listener, const std::string& state, int seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::string code;
  while (code.empty()) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd waiting{.fd = listener, .events = POLLIN, .revents = 0};
    if (left.count() <= 0 ||
        poll(&waiting, 1, static_cast<int>(left.count())) <= 0) {
      break;
    }
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) continue;
    char buffer[4096];
    pollfd reading{.fd = client, .events = POLLIN, .revents = 0};
    const ssize_t count =
        poll(&reading, 1, 2000) > 0 ? read(client, buffer, sizeof(buffer)) : 0;
    // The request line: GET /callback?code=…&state=… HTTP/1.1
    std::string line(buffer, count > 0 ? static_cast<size_t>(count) : 0);
    line.resize(std::min(line.find('\r'), line.size()));
    auto value = [&](std::string_view name) {
      const size_t at = line.find(std::string(name) + "=");
      if (at == std::string::npos) return std::string();
      const size_t from = at + name.size() + 1;
      return line.substr(from, line.find_first_of("& ", from) - from);
    };
    if (line.starts_with("GET /callback?") && value("state") == state) {
      code = value("code");
    }
    const std::string page =
        code.empty() ? "Nothing to do here." : "Signed in. Back to µAgent.";
    const std::string reply =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Connection: close\r\nContent-Length: " +
        std::to_string(page.size()) + "\r\n\r\n" + page;
    (void)!write(client, reply.data(), reply.size());
    close(client);
  }
  close(listener);
  return code;
}

}  // namespace uagent
