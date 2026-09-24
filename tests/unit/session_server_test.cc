// Copyright 2026 Timon Gentzsch
#include <filesystem>
#include <string>

#include "include/app/session.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/tools/files.h"
#include "tests/unit/test_support.h"

namespace uagent {

// A worker reports the executable it started from in its hello, so a host can
// recycle workers left running across an upgrade.
void TestWorkerBinaryIdentity() {
  TestWorkspace test("worker-binary");
  const std::filesystem::path probe = test.workspace / "uagent-probe";
  CHECK(ToolWriteFile(probe.string(), "v1").Ok());
  // A copy: SetExecutablePath replaces the string ExecutablePath refers to.
  const std::string prior =
      ExecutablePath();  // NOLINT(performance-unnecessary-copy-initialization)
  SetExecutablePath(probe.string());
  const std::string session_path = (test.workspace / "s.json").string();
  // Open creates the private socket folder before a worker starts its server.
  CreatePrivateDirectories(
      std::filesystem::path(session::SocketPath(session_path)).parent_path());
  {
    session::Server server;
    CHECK(server.Start(session_path, session::RandomToken(16),
                       [](const json&) { return true; }));
    session::Connection connection = session::Connect(session_path);
    CHECK(connection.socket.Valid());
    CHECK(!connection.binary.empty());
    CHECK(connection.binary == FileIdentity(probe.string()));
    // A same-second reinstall still changes the identity the host compares.
    CHECK(ToolWriteFile(probe.string(), "v1!").Ok());
    CHECK(FileIdentity(probe.string()) != connection.binary);
  }
  SetExecutablePath(prior);
}

}  // namespace uagent
