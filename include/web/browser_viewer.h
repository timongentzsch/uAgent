// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_BROWSER_VIEWER_H_
#define UAGENT_INCLUDE_WEB_BROWSER_VIEWER_H_
#include <cstdint>
#include <string>
namespace httplib::ws {
class WebSocket;
}
namespace uagent::web {
uint64_t RelayBrowserViewer(httplib::ws::WebSocket& socket,
                            const std::string& device,
                            const std::string& role);
}
#endif
