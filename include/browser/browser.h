// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_BROWSER_BROWSER_H_
#define UAGENT_INCLUDE_BROWSER_BROWSER_H_

#include <sys/types.h>

#include <cstdint>
#include <string>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent::browser {

// Profile flushes get a bounded grace period before a child is killed.
inline constexpr int kChildShutdownGraceMs = 5000;
// Tabs the agent is shown; more than these it cannot tell apart anyway.
inline constexpr size_t kListedTabs = 16;
// Chrome and Xvnc may each consume their grace; allow another for cleanup.
inline constexpr int kServiceShutdownGraceMs = 3 * kChildShutdownGraceMs;

// How long a caller waits on the service. A probe answers at once or not at
// all; an action may drive the page, and the service allows itself the same
// for reading a request and writing its answer.
inline constexpr int kRequestDefaultMs = 15000;
inline constexpr int kProbeMs = 1000;
inline constexpr int kActionMs = 30000;
// One read or write on the remote-framebuffer socket, and on Chrome's pipe.
inline constexpr int kRfbIoMs = 2000;
inline constexpr int kCdpIoMs = 10000;
// A new service answers its first ping, and Xvnc binds its socket, within.
inline constexpr int kServiceStartMs = 2000;
inline constexpr int kPingMs = 100;
inline constexpr int kRfbSocketWaitMs = 3000;
// How much of a page's text one read carries; the page script cuts there.
inline constexpr int kPageTextChars = 12000;

// The appliance sets one private directory for its browser profiles.
// An empty value disables the browser feature in ordinary native installs.
std::string DataDirectory();
// The web host resolves its own value from config at startup; everything
// else reads UAGENT_BROWSER_DATA from the environment.
void SetDataDirectory(std::string path);
std::string SocketPath();
std::string RfbPath();
// The site of an HTTP(S) address, as scheme://host[:port]: lower case, with
// no user part and no default port. Empty for any other address. What a
// remembered approval of a browser action covers.
std::string Origin(const std::string& url);
bool EnsureDataDirectory(const std::string& path);

struct ServiceProcess {
  ~ServiceProcess();
  pid_t pid = -1;
  Fd owner;
};

// Start the small browser owner alongside the web host. Chrome and Xvnc stay
// stopped until the first browser action or human takeover.
bool StartService(const std::string& executable, ServiceProcess& process,
                  std::string& error);
int ServiceMain(int owner_fd);

// One bounded request/reply per local Unix connection. No generic RPC surface
// or caller-selected CDP method is exposed by the service.
json Request(const json& command, int timeout_ms = kRequestDefaultMs);

}  // namespace uagent::browser

#endif  // UAGENT_INCLUDE_BROWSER_BROWSER_H_
