// Copyright 2026 Timon Gentzsch
#include <string>

#include "src/web/master_internal.h"
namespace uagent::web::master {
void Master::RegisterRemoteRoutes() {
  server_.Get("/api/hosts", [this](const Request&, Response& response) {
    Reply(response, {{"hosts", remotes_->List()}});
  });
  server_.Post("/api/hosts",
               [this](const Request& request, Response& response) {
                 if (request.body.size() > KiB(4)) {
                   Error(response, "host request exceeds limit", 413);
                   return;
                 }
                 const json action = json::parse(request.body, nullptr, false);
                 std::string error;
                 json result = remotes_->Change(action, error);
                 if (!error.empty()) {
                   Error(response, error);
                 } else {
                   Reply(response, result);
                 }
               });
  const std::string pattern =
      R"(/api/hosts/([a-f0-9]{32})(/(?:api|preview)/.*))";
  auto relay = [this](const Request& request, Response& response,
                      const httplib::ContentReader* reader) {
    const std::string id = request.matches[1], path = request.matches[2];
    // The gateway owns its device and host list. A remote may not turn this
    // relay into a second host manager or expose pairing/instance endpoints.
    if (path == "/api/auth" || path == "/api/instance" ||
        path.starts_with("/api/hosts")) {
      Error(response, "host-owned endpoint", 403);
      return;
    }
    const auto endpoint = remotes_->Endpoint(id);
    if (!endpoint) {
      Error(response, "Host is disconnected. Choose Connect or Reconnect.",
            503);
      return;
    }
    const size_t query = request.target.find('?');
    ProxyHttp(
        *endpoint, "/api/hosts/" + id,
        path + (query == std::string::npos ? "" : request.target.substr(query)),
        request, response, *ExpectedOrigin(request), reader);
  };
  server_.Get(R"(/preview/hosts/([a-f0-9]{32})(/.*))",
              [this](const Request& request, Response& response) {
                const std::string id = request.matches[1];
                const auto endpoint = remotes_->Endpoint(id);
                if (!endpoint) {
                  Error(response, "Host is disconnected", 503);
                  return;
                }
                const size_t query = request.target.find('?');
                ProxyHttp(*endpoint, "/api/hosts/" + id,
                          "/preview" + request.matches[2].str() +
                              (query == std::string::npos
                                   ? ""
                                   : request.target.substr(query)),
                          request, response, *ExpectedOrigin(request));
              });
  auto buffered = [relay](const Request& request, Response& response) {
    relay(request, response, nullptr);
  };
  auto streamed = [relay](const Request& request, Response& response,
                          const httplib::ContentReader& reader) {
    relay(request, response, &reader);
  };
  server_.Get(pattern, buffered);
  server_.Post(pattern, streamed);
  server_.Put(pattern, streamed);
  server_.Patch(pattern, streamed);
  server_.Delete(pattern, streamed);
  server_.Options(pattern, buffered);
  server_.WebSocket(pattern, [this](const Request& request,
                                    httplib::ws::WebSocket& socket) {
    if (WebSocketDevice(request).empty()) {
      socket.close(httplib::ws::CloseStatus::PolicyViolation);
      return;
    }
    const auto endpoint = remotes_->Endpoint(request.matches[1]);
    if (!endpoint) {
      socket.close();
      return;
    }
    const std::string path = request.matches[2];
    if (!path.starts_with("/api/terminals/") && path != "/api/browser/viewer") {
      socket.close();
      return;
    }
    const size_t query = request.target.find('?');
    ProxySocket(
        *endpoint,
        path + (query == std::string::npos ? "" : request.target.substr(query)),
        socket);
  });
}
}  // namespace uagent::web::master
