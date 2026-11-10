// Copyright 2026 Timon Gentzsch
#include <string>

#include "src/web/master_internal.h"
namespace uagent::web::master {
void Master::RegisterRemoteRoutes() {
  server_.Get("/api/hosts/([0-9a-f]{32})/ping", [this](const Request& request,
                                                       Response& response) {
    const json result = remotes_->Ping(request.matches[1].str());
    if (result.contains("error")) {
      Error(response, JsonValue(result, "error", "Measurement unavailable"),
            503);
    } else {
      Reply(response, result);
    }
  });
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
  host_.SetRemoteTarget([this](const std::string& id, const std::string& cwd,
                               const std::string& runtime) {
    return remotes_->Target(id, cwd, runtime);
  });
}
}  // namespace uagent::web::master
