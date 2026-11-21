// Copyright 2026 Timon Gentzsch

#include "include/app/openrouter_signin.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <string>

#include "include/api/client.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
namespace {

// OpenRouter's site, where the API is served under /api/v1.
std::string Site(std::string base_url) {
  constexpr std::string_view kApi = "/api/v1";
  if (base_url.ends_with(kApi)) base_url.resize(base_url.size() - kApi.size());
  return base_url;
}

}  // namespace

std::string PkceChallenge(std::string_view verifier) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(verifier.data()),
         verifier.size(), digest);
  return Base64Url({reinterpret_cast<const char*>(digest), sizeof digest});
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
