// Copyright 2026 Timon Gentzsch
#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT
#include <httplib.h>
#include <sys/socket.h>

#include <atomic>
#include <string>
#include <thread>

#include "include/core/fs.h"
#include "include/web/remote_proxy.h"
#include "tests/unit/test_support.h"
namespace uagent {
TEST_CASE(TestRemoteProxyStreamsAndKeepsCredentialsHostOwned) {
  TestWorkspace workspace("remote-proxy");
  const std::string socket_path = RuntimeDir() + "/proxy-test.sock";
  CHECK(EnsurePrivateDirectory(RuntimeDir()));
  unlink(socket_path.c_str());
  httplib::Server upstream, gateway;
  gateway.set_default_headers(
      {{"Content-Security-Policy", "default-src 'none'"},
       {"Cross-Origin-Resource-Policy", "same-origin"}});
  upstream.set_address_family(AF_UNIX);
  upstream.set_pre_routing_handler(
      [](const httplib::Request& request, httplib::Response& response) {
        if (request.get_header_value("Cookie") != "remote=private" ||
            request.get_header_value("Origin") != "http://localhost") {
          response.status = 403;
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });
  const std::string data =
      "{\n  \"base\": \"/preview/grant/\", \"url\": \"/api/download\"\n}\n";
  upstream.Get("/api/example", [&](const auto&, auto& response) {
    response.set_content(data, "application/json");
  });
  upstream.Get("/preview/grant/data.json", [&](const auto&, auto& response) {
    response.set_content(data, "application/json");
  });
  upstream.Get("/api/sessions/test/assets/data",
               [&](const auto&, auto& response) {
                 response.set_content(data, "application/json");
               });
  upstream.Post("/api/sessions/test/preview", [](const auto&, auto& response) {
    response.set_content(
        R"({"base":"/preview/grant/","url":"/api/download","text":"http://localhost/api/unchanged"})",
        "application/json");
  });
  upstream.Get("/preview/grant/index.html", [](const auto&, auto& response) {
    response.set_header("Content-Security-Policy",
                        "default-src http://localhost/preview/grant/; "
                        "frame-ancestors http://localhost");
    response.set_header("Cross-Origin-Resource-Policy", "cross-origin");
    response.set_content("<p>Remote preview</p>", "text/html");
  });
  const std::string bytes =
      std::string(MiB(2), 'x') + std::string("\0\xff\x01", 3);
  upstream.Post("/api/upload", [&](const auto& request, auto& response) {
    response.set_content(request.body == bytes ? "yes" : "no", "text/plain");
  });
  upstream.Get("/api/download", [&](const auto&, auto& response) {
    response.set_content(bytes, "application/octet-stream");
  });
  std::atomic<bool> release = false;
  upstream.Get("/api/events", [&](const auto&, auto& response) {
    response.set_chunked_content_provider(
        "text/event-stream", [&](size_t offset, httplib::DataSink& sink) {
          if (!offset) return sink.write("event: ready\ndata: {}\n\n", 23);
          while (!release && sink.is_writable()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          sink.done();
          return true;
        });
  });
  upstream.WebSocket("/api/socket", [](const auto&, auto& socket) {
    std::string message;
    for (;;) {
      const auto kind = socket.read(message);
      if (kind == httplib::ws::ReadResult::Fail) break;
      if (kind == httplib::ws::ReadResult::Binary) {
        socket.send(message.data(), message.size());
      } else {
        socket.send(message);
      }
    }
  });
  CHECK(upstream.bind_to_port(socket_path, 80));
  const web::RemoteEndpoint endpoint{"http://localhost", socket_path,
                                     "remote=private"};
  auto relay = [&](const auto& request, auto& response) {
    web::ProxyHttp(endpoint, "/api/hosts/owner", request.target, request,
                   response, "https://serving.example");
  };
  gateway.Get(R"(/api/.*)", relay);
  gateway.Get(R"(/preview/.*)", relay);
  gateway.Post(R"(/api/.*)", [&](const httplib::Request& request,
                                 httplib::Response& response,
                                 const httplib::ContentReader& reader) {
    web::ProxyHttp(endpoint, "/api/hosts/owner", request.target, request,
                   response, "https://serving.example", &reader);
  });
  gateway.WebSocket("/api/socket", [&](const auto&, auto& socket) {
    web::ProxySocket(endpoint, "/api/socket", socket);
  });
  const int port = gateway.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  std::thread remote([&] { upstream.listen_after_bind(); });
  std::thread serving([&] { gateway.listen_after_bind(); });
  httplib::Client client("127.0.0.1", port);
  client.set_read_timeout(5);
  auto result = client.Get(
      "/api/example", httplib::Headers{{"Cookie", "local=never-forward"},
                                       {"Origin", "https://serving.example"}});
  CHECK(result && result->status == 200);
  if (result) {
    CHECK(result->body == data);
    CHECK(!result->has_header("Set-Cookie"));
  }
  for (const auto* path :
       {"/preview/grant/data.json", "/api/sessions/test/assets/data"}) {
    auto file = client.Get(path);
    CHECK(file && file->status == 200 && file->body == data);
    auto range = client.Get(path, httplib::Headers{{"Range", "bytes=2-15"}});
    CHECK(range && range->status == 206 && range->body == data.substr(2, 14));
  }
  auto grant = client.Post("/api/sessions/test/preview?check=1", "{}",
                           "application/json");
  CHECK(grant && grant->status == 200);
  if (grant) {
    const json body = json::parse(grant->body, nullptr, false);
    CHECK(JsonValue(body, "base", "") == "/preview/hosts/owner/grant/");
    CHECK(JsonValue(body, "url", "") == "/api/download");
    CHECK(JsonValue(body, "text", "") == "http://localhost/api/unchanged");
    CHECK(!grant->has_header("Set-Cookie"));
  }
  auto preview = client.Get("/preview/grant/index.html");
  CHECK(preview && preview->status == 200);
  if (preview) {
    CHECK(preview->get_header_value_count("Content-Security-Policy") == 1);
    CHECK(preview->get_header_value("Content-Security-Policy") ==
          "default-src https://serving.example/preview/hosts/owner/grant/; "
          "frame-ancestors https://serving.example");
    CHECK(preview->get_header_value("Cross-Origin-Resource-Policy") ==
          "cross-origin");
  }
  auto upload = client.Post("/api/upload", bytes, "application/octet-stream");
  CHECK(upload && upload->body == "yes");
  auto download = client.Get("/api/download");
  CHECK(download && download->body == bytes);
  auto range =
      client.Get("/api/download", httplib::Headers{{"Range", "bytes=10-29"}});
  CHECK(range && range->status == 206 && range->body == bytes.substr(10, 20));
  std::atomic<bool> streamed = false;
  std::thread stream([&] {
    client.Get("/api/events", [&](const char* data, size_t length) {
      streamed =
          std::string(data, length).find("event: ready") != std::string::npos;
      return false;
    });
  });
  // The first frame must arrive while the producer is still open.
  CHECK(WaitUntil([&] { return streamed.load(); }, BudgetMs(5000)));
  release = true;
  stream.join();
  {
    httplib::ws::WebSocketClient viewer(
        "ws://127.0.0.1:" + std::to_string(port) + "/api/socket");
    viewer.set_read_timeout(5);
    CHECK(static_cast<bool>(viewer.connect()));
    CHECK(viewer.send("text"));
    std::string echoed;
    CHECK(viewer.read(echoed) == httplib::ws::ReadResult::Text &&
          echoed == "text");
    const std::string binary("\0\xff\x01", 3);
    CHECK(viewer.send(binary.data(), binary.size()));
    CHECK(viewer.read(echoed) == httplib::ws::ReadResult::Binary &&
          echoed == binary);
    viewer.close();
  }
  gateway.stop();
  upstream.stop();
  serving.join();
  remote.join();
  unlink(socket_path.c_str());
}
}  // namespace uagent
