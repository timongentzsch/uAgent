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
#include "include/app/session_host.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/tools/files.h"
#include "tests/unit/test_support.h"

namespace uagent {

TEST_CASE(TestWorkerRestartWaitsForActivityAndApprovals) {
  session::HostSession worker;
  CHECK(!worker.RestartBusy());
  worker.turn_active = true;
  CHECK(worker.RestartBusy());
  worker.turn_active = false;
  worker.command_busy = true;
  CHECK(worker.RestartBusy());
  worker.command_busy = false;
  worker.pending = {{"request_id", "approval"}};
  CHECK(worker.RestartBusy());
  worker.pending = nullptr;
  worker.active_exchanges["tool"] = json::object();
  CHECK(worker.RestartBusy());
  worker.active_exchanges.clear();
  CHECK(!worker.RestartBusy());
  worker.state["activities"] = json::array({{{"status", "running"}}});
  CHECK(worker.RestartBusy());
  worker.state["activities"][0]["status"] = "finished";
  CHECK(!worker.RestartBusy());
}

// A worker reports the executable it started from in its hello, so a host can
// recycle workers left running across an upgrade.
TEST_CASE(TestWorkerBinaryIdentity) {
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
TEST_CASE(TestCloseRuntimeWaitsForTheRuntime) {
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
    CHECK(WaitUntil([&] { return closing.load(); }, BudgetMs(5000)));
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

// Closing with frames unread resets the socket; the command is taken anyway.
TEST_CASE(TestACommandSentBeforeClosingIsTaken) {
  TestWorkspace test("send-and-close");
  const std::string path = (test.workspace / "s.json").string();
  CreatePrivateDirectories(
      std::filesystem::path(session::SocketPath(path)).parent_path());
  std::atomic<bool> taken{false};
  session::Server server;
  CHECK(server.Start(path, RandomToken(16), [&](const json& frame) {
    taken = taken || JsonValue(frame, "kind", "") == "interrupt";
    return true;
  }) == 0);
  // What a joining client is sent and, here, never reads.
  server.Publish(
      {{"v", session::kProtocol}, {"kind", "state"}, {"busy", true}});
  {
    session::Connection connection = session::Connect(path);
    CHECK(session::WriteFrame(
        connection.socket.Get(),
        {{"v", session::kProtocol}, {"kind", "interrupt"}}));
  }
  CHECK(WaitUntil([&] { return taken.load(); }, BudgetMs(5000)));
}

// What is published while the server is stopping reaches its clients: a
// runtime that cannot start says why to a terminal that has just connected.
// The command handler runs after the server has taken what was published,
// so it publishes there and stops the server before handing back.
TEST_CASE(TestAFramePublishedAsTheServerStopsIsDelivered) {
  TestWorkspace test("publish-and-stop");
  const std::string path = (test.workspace / "s.json").string();
  CreatePrivateDirectories(
      std::filesystem::path(session::SocketPath(path)).parent_path());
  int delivered = 0;
  constexpr int kRounds = 50;
  for (int round = 0; round < kRounds; ++round) {
    auto server = std::make_unique<session::Server>();
    std::atomic<bool> stopping{false};
    std::thread stopper;
    CHECK(server->Start(path, RandomToken(16), [&](const json&) {
      server->Publish(
          {{"v", session::kProtocol}, {"kind", "error"}, {"error", "why"}});
      stopper = std::thread([&] {
        stopping = true;
        server.reset();
      });
      while (!stopping) std::this_thread::yield();
      return true;
    }) == 0);
    session::Connection connection = session::Connect(path);
    REQUIRE(connection.socket.Valid());
    CHECK(
        session::WriteFrame(connection.socket.Get(),
                            {{"v", session::kProtocol}, {"kind", "refresh"}}));
    session::ReadFrames(connection.socket.Get(), -1, session::kFrameBytes,
                        [&](const json& frame) {
                          delivered += JsonValue(frame, "kind", "") == "error";
                          return true;
                        });
    stopper.join();
  }
  CHECK(delivered == kRounds);
}

TEST_CASE(TestSessionFramePartitions) {
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
