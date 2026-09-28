// Copyright 2026 Timon Gentzsch
// The web host without the browser appliance (UAGENT_BROWSER=OFF): no data
// directory, so it offers no browser and no session gets the tool.

#include <cstdio>
#include <string>

#include "include/browser/browser.h"
#include "include/web/browser_viewer.h"

namespace uagent {
namespace browser {

std::string DataDirectory() { return ""; }

void SetDataDirectory(std::string) {
  fprintf(stderr, "this build has no browser appliance; ignoring its data\n");
}

ServiceProcess::~ServiceProcess() = default;

bool StartService(const std::string&, int64_t, ServiceProcess&, std::string&) {
  return true;
}

int ServiceMain(int) {
  fprintf(stderr, "this build has no browser appliance\n");
  return 1;
}

json Request(const json&, int) {
  return {{"error", "this build has no browser appliance"}};
}

}  // namespace browser

namespace web {
ViewerSession RelayBrowserViewer(httplib::ws::WebSocket&, const std::string&) {
  return {};
}
}  // namespace web
}  // namespace uagent
