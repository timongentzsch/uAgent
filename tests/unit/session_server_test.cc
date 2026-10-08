// Copyright 2026 Timon Gentzsch
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/app/launch.h"
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
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const std::string prior = ExecutablePath();
  SetExecutablePath(probe.string());
  const std::string session_path = (test.workspace / "s.json").string();
  // Open creates the private socket folder before a worker starts its server.
  CreatePrivateDirectories(
      std::filesystem::path(session::SocketPath(session_path)).parent_path());
  {
    session::Server server;
    CHECK(server.Start(session_path, RandomToken(16),
                       [](const json&) { return true; }) == 0);
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

// Closing a runtime waits for the runtime, not for its socket file: one that
// crashed leaves the file behind and is gone all the same.
void TestCloseRuntimeWaitsForTheRuntime() {
  TestWorkspace test("close-runtime");
  const std::string path = (test.workspace / "s.json").string();
  const std::string address = session::SocketPath(path);
  CreatePrivateDirectories(std::filesystem::path(address).parent_path());
  auto server = std::make_unique<session::Server>();
  std::atomic<bool> closing{false};
  CHECK(server->Start(path, RandomToken(16), [&](const json& frame) {
    if (JsonValue(frame, "kind", "") != "close") return true;
    server->Publish({{"v", session::kProtocol},
                     {"kind", "outcome"},
                     {"request_id", JsonValue(frame, "request_id", "")},
                     {"accepted", true}});
    closing = true;
    return true;
  }) == 0);
  server->Publish(
      {{"v", session::kProtocol}, {"kind", "state"}, {"busy", false}});
  std::thread runtime([&] {
    while (!closing) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    server.reset();
  });
  CHECK(CloseRuntime(path).empty());
  runtime.join();
  CHECK(!session::Connect(path).socket);

  // What a killed runtime leaves: a socket nobody listens on.
  {
    const Fd abandoned = ListenUnix(address, 1);
  }
  CHECK(PathExists(address));
  const auto began = std::chrono::steady_clock::now();
  CHECK(CloseRuntime(path).empty());
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(1));
  unlink(address.c_str());
}

void TestSessionFramePartitions() {
  const json first = {{"v", session::kProtocol}, {"text", "a\nbλ"}};
  const json second = {{"v", session::kProtocol}, {"sequence", 2}};
  const std::string wire = JsonDump(first) + "\n" + JsonDump(second) + "\n";
  for (size_t split = 0; split <= wire.size(); ++split) {
    session::FrameBuffer buffer;
    std::vector<json> frames;
    auto receive = [&](json value) {
      frames.push_back(std::move(value));
      return true;
    };
    CHECK(buffer.Feed(std::string_view(wire).substr(0, split), receive));
    CHECK(buffer.Feed(std::string_view(wire).substr(split), receive));
    CHECK((frames == std::vector<json>{first, second}));
  }
  for (const std::string& invalid : std::vector<std::string>{
           "[]\n", "{bad}\n", "{\"v\":1}\n", "\n", std::string(65, 'x')}) {
    session::FrameBuffer buffer(64);
    CHECK(!buffer.Feed(invalid, [](const json&) {
      CHECK(false);
      return true;
    }));
  }
  session::FrameBuffer buffer;
  int calls = 0;
  CHECK(!buffer.Feed(wire, [&](const json&) {
    ++calls;
    return false;
  }));
  CHECK(calls == 1);

  // The blocking adapter drains buffered frames, obeys a wake descriptor,
  // and terminates on a deadline without requiring a socket or provider.
  session::Pipe stream, stop;
  REQUIRE(stream.Open());
  REQUIRE(stop.Open());
  CHECK(session::WriteFrame(stream.write.Get(), first));
  calls = 0;
  session::ReadFrames(stream.read.Get(), stop.read.Get(), 1024,
                      [&](const json& value) {
                        CHECK(value == first);
                        ++calls;
                        return false;
                      });
  CHECK(calls == 1);
  stop.Wake();
  session::ReadFrames(stream.read.Get(), stop.read.Get(), 1024,
                      [](const json&) {
                        CHECK(false);
                        return true;
                      });
  stop.Drain();
  session::ReadFrames(
      stream.read.Get(), -1, 1024,
      [](const json&) {
        CHECK(false);
        return true;
      },
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
}

}  // namespace uagent
