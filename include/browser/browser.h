// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_BROWSER_BROWSER_H_
#define UAGENT_INCLUDE_BROWSER_BROWSER_H_

#include <sys/types.h>

#include <string>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent::browser {

// The appliance sets one private directory for its single browser profile.
// An empty value disables the browser feature in ordinary native installs.
std::string DataDirectory();
std::string SocketPath();
std::string RfbPath();
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
json Request(const json& command, int timeout_ms = 15000);

}  // namespace uagent::browser

#endif  // UAGENT_INCLUDE_BROWSER_BROWSER_H_
