// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_BROWSER_VIEWER_H_
#define UAGENT_INCLUDE_WEB_BROWSER_VIEWER_H_
#include <cstdint>
#include <string>
namespace httplib::ws {
class WebSocket;
}
namespace uagent::web {
// The display a viewer served and the lease generation it last saw; a zero
// display when it could not start.
struct ViewerSession {
  uint64_t display = 0;
  uint64_t generation = 0;
};
// Relays one paired device's viewer to the browser display. Input reaches
// Chrome only while that device controls the browser.
ViewerSession RelayBrowserViewer(httplib::ws::WebSocket& socket,
                                 const std::string& device);
}  // namespace uagent::web
#endif
