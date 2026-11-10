// Copyright 2026 Timon Gentzsch
#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT
#include "include/web/remote_proxy.h"

#include <httplib.h>
#include <sys/socket.h>

#include <array>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent::web {
namespace {
httplib::Headers Headers(const RemoteEndpoint& endpoint,
                         const httplib::Request& request) {
  httplib::Headers headers{{"Cookie", endpoint.cookie},
                           {"Origin", endpoint.origin}};
  for (const char* key : {"Content-Type", "Range", "If-Range", "If-None-Match",
                          "If-Match", "Accept", "Last-Event-ID"}) {
    if (request.has_header(key)) {
      headers.emplace(key, request.get_header_value(key));
    }
  }
  return headers;
}
}  // namespace
void ProxyHttp(const RemoteEndpoint& endpoint, const std::string& prefix,
               const std::string& path, const httplib::Request& request,
               httplib::Response& response, const std::string& browser_origin,
               const httplib::ContentReader* reader) {
  auto client = std::make_shared<httplib::Client>(endpoint.origin);
  client->set_connection_timeout(5);
  client->set_read_timeout(30);
  client->set_write_timeout(30);
  client->set_follow_location(false);
  client->set_decompress(false);
  if (!endpoint.socket.empty()) {
    client->set_address_family(AF_UNIX);
    client->set_hostname_addr_map({{client->host(), endpoint.socket}});
  }
  httplib::ContentProvider upload;
  if (reader) {
    upload = [reader](size_t, size_t, httplib::DataSink& sink) {
      const bool complete =
          (*reader)([&sink](const char* bytes, size_t length) {
            return sink.write(bytes, length);
          });
      if (complete) sink.done();
      return complete;
    };
  }
  auto stream =
      std::make_shared<httplib::ClientImpl::StreamHandle>(client->open_stream(
          request.method, path, {}, Headers(endpoint, request), request.body,
          request.get_header_value("Content-Type"), std::move(upload), 0,
          reader != nullptr));
  if (!stream->is_valid()) {
    if (endpoint.unavailable) endpoint.unavailable();
    response.status = 502;
    response.set_content(
        "{\"error\":\"Remote connection unavailable; reconnect the host.\"}",
        "application/json");
    return;
  }
  response.status = stream->response->status;
  response.ranges_applied = true;
  if (response.status == 401 && endpoint.unavailable) endpoint.unavailable();
  for (const char* key :
       {"Content-Type", "Content-Disposition", "Content-Range", "Accept-Ranges",
        "ETag", "Content-Security-Policy", "X-Content-Type-Options",
        "Referrer-Policy", "Cross-Origin-Resource-Policy",
        "Access-Control-Allow-Origin"}) {
    if (stream->response->has_header(key)) {
      response.headers.erase(key);
      response.set_header(key, stream->response->get_header_value(key));
    }
  }
  if (path.starts_with("/preview/")) {
    std::string policy = response.get_header_value("Content-Security-Policy");
    const std::string& destination = browser_origin;
    const std::string from = endpoint.origin + "/preview/";
    const std::string to =
        destination + "/preview/hosts/" + prefix.substr(11) + "/";
    size_t at = 0;
    while ((at = policy.find(from, at)) != std::string::npos) {
      policy.replace(at, from.size(), to);
      at += to.size();
    }
    at = 0;
    while ((at = policy.find(endpoint.origin, at)) != std::string::npos) {
      policy.replace(at, endpoint.origin.size(), destination);
      at += destination.size();
    }
    response.headers.erase("Content-Security-Policy");
    response.set_header("Content-Security-Policy", policy);
  }
  if (request.method == "HEAD" || response.status == 204 ||
      response.status == 304) {
    if (stream->response->has_header("Content-Length")) {
      response.set_header("Content-Length",
                          stream->response->get_header_value("Content-Length"));
    }
    return;
  }
  const std::string mime = stream->response->get_header_value("Content-Type");
  // Only the preview grant contains a server-minted address to translate.
  // Every other response, including JSON assets and tool data, stays opaque.
  const std::string route = path.substr(0, path.find('?'));
  if (request.method == "POST" && route.starts_with("/api/sessions/") &&
      route.ends_with("/preview") && mime.starts_with("application/json")) {
    std::array<char, 16384> bytes;
    std::string body;
    ssize_t count = 0;
    while ((count = stream->read(bytes.data(), bytes.size())) > 0) {
      if (body.size() + static_cast<size_t>(count) > MiB(16)) {
        response.status = 502;
        response.set_content("{\"error\":\"Remote response exceeds limit.\"}",
                             "application/json");
        return;
      }
      body.append(bytes.data(), static_cast<size_t>(count));
    }
    if (count < 0) {
      if (endpoint.unavailable) endpoint.unavailable();
      response.status = 502;
      body = "{\"error\":\"Remote response interrupted.\"}";
    }
    json parsed = json::parse(body, nullptr, false);
    if (!parsed.is_discarded()) {
      const std::string base = JsonValue(parsed, "base", "");
      if (base.starts_with("/preview/")) {
        parsed["base"] = "/preview/hosts/" + prefix.substr(11) + base.substr(8);
      }
      body = JsonDump(parsed);
    }
    response.set_content(std::move(body), mime);
    return;
  }
  // StreamHandle transfers socket ownership. Pull directly into the server's
  // bounded write buffer: no producer thread, full-body cache or extra queue.
  response.set_chunked_content_provider(
      mime, [stream, client, unavailable = endpoint.unavailable](
                size_t, httplib::DataSink& sink) {
        std::array<char, 16384> bytes;
        const ssize_t count = stream->read(bytes.data(), bytes.size());
        if (count < 0) {
          if (unavailable) unavailable();
          return false;
        }
        if (count == 0) {
          sink.done();
          return true;
        }
        return sink.write(bytes.data(), static_cast<size_t>(count));
      });
}
void ProxySocket(const RemoteEndpoint& endpoint, const std::string& path,
                 httplib::ws::WebSocket& downstream) {
  httplib::ws::WebSocketClient upstream(
      "ws" + endpoint.origin.substr(4) + path,
      {{"Origin", endpoint.origin}, {"Cookie", endpoint.cookie}});
  upstream.set_connection_timeout(5);
  // The peer pings every 30 seconds; leave room for its missed-pong budget.
  upstream.set_read_timeout(90);
  if (!endpoint.socket.empty()) {
    upstream.set_address_family(AF_UNIX);
    const std::string authority = endpoint.origin.substr(7);
    const std::string host = authority.substr(0, authority.find(':'));
    upstream.set_hostname_addr_map({{host, endpoint.socket}});
  }
  if (!upstream.connect()) {
    if (endpoint.unavailable) endpoint.unavailable();
    downstream.close(httplib::ws::CloseStatus::PolicyViolation);
    return;
  }
  std::thread returning([&] {
    std::string message;
    for (;;) {
      auto kind = upstream.read(message);
      if (kind == httplib::ws::ReadResult::Fail || message.size() > MiB(1)) {
        break;
      }
      const bool sent = kind == httplib::ws::ReadResult::Binary
                            ? downstream.send(message.data(), message.size())
                            : downstream.send(message);
      if (!sent) break;
    }
    downstream.close();
  });
  std::string message;
  for (;;) {
    auto kind = downstream.read(message);
    if (kind == httplib::ws::ReadResult::Fail || message.size() > MiB(1)) break;
    const bool sent = kind == httplib::ws::ReadResult::Binary
                          ? upstream.send(message.data(), message.size())
                          : upstream.send(message);
    if (!sent) break;
  }
  upstream.close();
  downstream.close();
  returning.join();
}
}  // namespace uagent::web
