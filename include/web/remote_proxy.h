// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_REMOTE_PROXY_H_
#define UAGENT_INCLUDE_WEB_REMOTE_PROXY_H_
#include <functional>
#include <string>
namespace httplib {
struct Request;
class ContentReader;
struct Response;
namespace ws {
class WebSocket;
}
}  // namespace httplib
namespace uagent::web {
struct RemoteEndpoint {
  std::string origin, socket, cookie;
  std::function<void()> unavailable = {};
};
// Only the owning host manager supplies an endpoint and stripped API path.
void ProxyHttp(const RemoteEndpoint& endpoint, const std::string& prefix,
               const std::string& path, const httplib::Request& request,
               httplib::Response& response,
               const std::string& browser_origin = "",
               const httplib::ContentReader* reader = nullptr);
void ProxySocket(const RemoteEndpoint& endpoint, const std::string& path,
                 httplib::ws::WebSocket& downstream);
}  // namespace uagent::web
#endif
