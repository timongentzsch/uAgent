// Copyright 2026 Timon Gentzsch
#include <unistd.h>

#include <filesystem>
#include <string>

#include "include/app/session.h"
#include "include/tools/files.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestWorkerBinaryIdentity() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() /
                        ("uagent-worker-binary-test-" +
                         std::to_string(static_cast<int64_t>(getpid())));
  std::error_code ec;
  fs::create_directories(root, ec);
  const std::string session = (root / "s.json").string();
  const fs::path probe = root / "uagent";

  // Missing record (pre-feature spawn) always recycles, exactly once:
  // after recording, the same binary is fresh.
  CHECK(session::WorkerBinaryPath(session).ends_with(".sock.binary"));
  CHECK(session::ReadWorkerBinary(session).empty());
  CHECK(session::WorkerBinaryStale("current", ""));
  {
    std::string error;
    CHECK(ToolWriteFile(probe.string(), "v1").Ok());
    const std::string first = session::ExecutableIdentity(probe.string());
    CHECK(!first.empty());
    CHECK(session::WriteWorkerBinary(session, first));
    CHECK(session::ReadWorkerBinary(session) == first);
    CHECK(
        !session::WorkerBinaryStale(first, session::ReadWorkerBinary(session)));
    // Same-second reinstalls still differ by size.
    CHECK(ToolWriteFile(probe.string(), "v1!").Ok());
    const std::string second = session::ExecutableIdentity(probe.string());
    CHECK(second != first);
    CHECK(session::WorkerBinaryStale(second, first));
  }
  // An unstatable executable never recycles blindly.
  CHECK(session::ExecutableIdentity((root / "missing").string()).empty());
  CHECK(!session::WorkerBinaryStale("", "anything"));
  fs::remove_all(root, ec);
}

}  // namespace uagent
