// Copyright 2026 Timon Gentzsch

#include "include/browser/browser.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "include/browser/broker.h"
#include "include/browser/runtime.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/lease.h"
#include "include/core/signals.h"
#include "include/tools/browser.h"
#include "include/transport/session.h"
#include "include/web/rfb_filter.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestBrowserInputFilter() {
  web::RfbInputFilter filter;
  std::string output;
  CHECK(filter.Push("RFB 003.", output, false));
  CHECK(output == "RFB 003.");
  CHECK(filter.Push(std::string("008\n\x01\x01", 6), output, false));
  CHECK(output == std::string("008\n\x01\x01", 6));

  const std::string update("\x03\x01\0\0\0\0\x05\0\x03\x20", 10);
  const std::string key("\x04\x01\0\0\0\0\0\x61", 8);
  const std::string pointer("\x05\x01\0\x10\0\x20", 6);
  const std::string clipboard(
      "\x06\0\0\0\0\0\0\x03"
      "abc",
      11);
  // Watching: only display messages pass.
  CHECK(
      filter.Push(update + key + pointer + clipboard + update, output, false));
  CHECK(output == update + update);
  CHECK(filter.Release().empty());

  const std::string encodings("\x02\0\0\x01\0\0\0\0", 8);
  CHECK(filter.Push(encodings.substr(0, 3), output, false));
  CHECK(output.empty());
  CHECK(filter.Push(encodings.substr(3), output, false));
  CHECK(output == encodings);

  // Control arrives mid-stream, inside a fragmented message: framing holds,
  // and the held key and button are lifted when control ends.
  const std::string held = key + pointer;
  CHECK(filter.Push(held.substr(0, 5), output, true));
  CHECK(output.empty());
  CHECK(filter.Push(held.substr(5), output, true));
  CHECK(output == held);
  CHECK(filter.Release() == std::string("\x04\0\0\0\0\0\0\x61", 8) +
                                std::string("\x05\0\0\x10\0\x20", 6));
  CHECK(filter.Release().empty());
  CHECK(filter.Push(key + update, output, false));
  CHECK(output == update);
  // A QEMU extended key is lifted in its own form, scan code included.
  const std::string qemu_down("\xff\0\0\x01\0\0\xff\xe1\0\0\0\x2a", 12);
  CHECK(filter.Push(qemu_down, output, true));
  CHECK(filter.Release() ==
        std::string("\xff\0\0\0\0\0\xff\xe1\0\0\0\x2a", 12));

  const std::string extended_clipboard(
      "\x06\0\0\0\xff\xff\xff\xfc"
      "data",
      12);
  CHECK(filter.Push(extended_clipboard, output, false));
  CHECK(output.empty());
  CHECK(!filter.Push(std::string("\x07", 1), output, true));
}

void TestBrowserHandoverRecovery() {
  TestWorkspace workspace("browser-handover");
  auto directory = std::filesystem::canonical(workspace.root) / "browser";
  ScopedEnv configured("UAGENT_BROWSER_DATA", directory.string());
  CHECK(browser::EnsureDataDirectory(directory.string()));
  constexpr const char* kSession = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr const char* kInteraction = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  {
    std::ofstream saved(directory / "handover.json");
    saved << JsonDump(
        {{"session_id", kSession}, {"interaction_id", kInteraction}});
  }
  browser::Runtime runtime;
  json status = runtime.Execute({{"op", "status"}});
  CHECK(status.value("mode", "") == "human");
  CHECK(!status.value("running", true));
  CHECK(runtime.Execute({{"op", "agent_status"}, {"session_id", kSession}})
            .contains("error"));
  CHECK(runtime
            .Execute({{"op", "cancel_handover"},
                      {"session_id", kSession},
                      {"interaction_id", "cccccccccccccccccccccccccccccccc"}})
            .contains("error"));
  CHECK(std::filesystem::exists(directory / "handover.json"));
  json released = runtime.Execute({{"op", "cancel_handover"},
                                   {"session_id", kSession},
                                   {"interaction_id", kInteraction}});
  CHECK(released.value("mode", "") == "idle");
  CHECK(!std::filesystem::exists(directory / "handover.json"));
  // A handover whose conversation no longer runs is dropped by the idle
  // check: nobody is left to hand the browser back to.
  {
    std::ofstream saved(directory / "handover.json");
    saved << JsonDump(
        {{"session_id", kSession}, {"interaction_id", kInteraction}});
  }
  browser::Runtime orphaned;
  CHECK(orphaned.Execute({{"op", "status"}}).value("mode", "") == "human");
  // While its conversation runs (it holds its socket's lease), it stays.
  {
    CreatePrivateDirectories(RuntimeDir());
    FileLease running;
    std::string error;
    CHECK(running.Acquire(session::SocketPathForId(kSession) + ".lock", error,
                          true));
    orphaned.StopIfIdle(std::chrono::minutes(kIdleMinutes));
    CHECK(orphaned.Execute({{"op", "status"}}).value("mode", "") == "human");
    // The service runs with its own HOME, so the host names the session's
    // runtime directory for it; without that the lease would look missing.
    {
      const std::string sessions = RuntimeDir();
      ScopedEnv named("UAGENT_INTERNAL_RUNTIME_DIR", sessions);
      ScopedEnv service_home("HOME", directory.string());
      CHECK(RuntimeDir() != sessions);
      orphaned.StopIfIdle(std::chrono::minutes(kIdleMinutes));
    }
    CHECK(orphaned.Execute({{"op", "status"}}).value("mode", "") == "human");
    // A runtime that exits removes its lease.
    std::filesystem::remove(session::SocketPathForId(kSession) + ".lock");
  }
  orphaned.StopIfIdle(std::chrono::minutes(kIdleMinutes));
  CHECK(orphaned.Execute({{"op", "status"}}).value("mode", "") == "idle");
  CHECK(!std::filesystem::exists(directory / "handover.json"));
}

void TestBrowserProfiles() {
  TestWorkspace workspace("browser-profiles");
  auto directory = std::filesystem::canonical(workspace.root) / "browser";
  ScopedEnv configured("UAGENT_BROWSER_DATA", directory.string());
  CHECK(browser::EnsureDataDirectory(directory.string()));
  CHECK(browser::EnsureDataDirectory((directory / "profile").string()));
  {
    std::ofstream legacy(directory / "profile" / "existing-login");
    legacy << "preserved";
  }
  constexpr const char* kDevice = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  std::string created_id;
  {
    browser::Runtime runtime;
    json initial = runtime.Execute({{"op", "status"}});
    CHECK(initial.value("profile_id", "") == "default");
    CHECK(initial["profiles"].size() == 1);
    CHECK(runtime
              .Execute({{"op", "create_profile"},
                        {"device", kDevice},
                        {"name", "Default"}})
              .contains("error"));
    CHECK(runtime
              .Execute({{"op", "create_profile"},
                        {"device", kDevice},
                        {"name", "bad\nname"}})
              .contains("error"));
    json created = runtime.Execute(
        {{"op", "create_profile"}, {"device", kDevice}, {"name", "  Work  "}});
    created_id = created.value("created_profile_id", "");
    CHECK(created["profiles"].size() == 2);
    CHECK(created["profiles"][1].value("name", "") == "Work");
    CHECK(runtime
              .Execute({{"op", "select_profile"},
                        {"device", kDevice},
                        {"profile_id", "missing"}})
              .contains("error"));
    CHECK(runtime
              .Execute({{"op", "select_profile"},
                        {"device", kDevice},
                        {"profile_id", created_id}})
              .value("profile_id", "") == created_id);
  }
  CHECK(std::filesystem::exists(directory / "profile" / "existing-login"));
  {
    browser::Runtime restored;
    json status = restored.Execute({{"op", "status"}});
    CHECK(status.value("profile_id", "") == created_id);
    CHECK(status["profiles"].size() == 2);
    CHECK(restored
              .Execute({{"op", "select_profile"},
                        {"device", kDevice},
                        {"profile_id", "default"}})
              .value("profile_id", "") == "default");
  }
  {
    std::ofstream corrupt(directory / "profiles.json", std::ios::trunc);
    corrupt << "invalid";
  }
  browser::Runtime corrupt;
  CHECK(corrupt.Execute({{"op", "status"}}).contains("error"));
  CHECK(corrupt
            .Execute({{"op", "create_profile"},
                      {"device", kDevice},
                      {"name", "Personal"}})
            .contains("error"));
}

namespace {
// The fake X server, VNC and Chrome from tests/fixtures, put on `bin` and
// found first, with the browser's data in `data` and no display of the
// machine's own.
struct FakeBrowser {
  FakeBrowser(const std::filesystem::path& bin,
              const std::filesystem::path& data)
      : configured("UAGENT_BROWSER_DATA", data.string()),
        search("PATH", Install(bin) + ":" + getenv("PATH")) {}
  static std::string Install(const std::filesystem::path& bin) {
    namespace fs = std::filesystem;
    fs::create_directory(bin);
    for (const char* name : {"xauth", "Xtigervnc", "google-chrome-stable"}) {
      const auto path = bin / name;
      fs::copy_file(fs::path(UAGENT_TEST_SOURCE_DIR) /
                        "tests/fixtures/browser_runtime.py",
                    path);
      fs::permissions(path, fs::perms::owner_all);
    }
    return bin.string();
  }
  ScopedEnv configured, search, display{"DISPLAY"}, authority{"XAUTHORITY"};
};
}  // namespace

// Scheduling is independent of page execution: FIFO admission, explicit
// cancellation, fencing and expiry can be exercised without starting Chrome.
void TestBrowserScheduling() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-queue");
  const auto directory = fs::canonical(workspace.root) / "browser";
  CHECK(browser::EnsureDataDirectory(directory.string()));
  auto now = std::chrono::steady_clock::now();
  browser::Broker broker(directory.string(), [&] { return now; });
  const auto acquire = [&](const std::string& id,
                           const std::string& request = "1111111111111111") {
    return broker.Execute(
        {{"op", "acquire"}, {"session_id", id}, {"request_id", request}});
  };
  constexpr const char* a = "aaaaaaaaaaaaaaaa";
  constexpr const char* b = "bbbbbbbbbbbbbbbb";
  constexpr const char* c = "cccccccccccccccc";
  constexpr const char* d = "dddddddddddddddd";
  auto first = acquire(a);
  CHECK(first.value("acquired", false));
  CHECK(first.value("instance_id", "") == "0");
  CHECK(acquire(b).value("position", 0) == 1);
  CHECK(acquire(c).value("position", 0) == 2);
  CHECK(acquire(d).value("position", 0) == 3);
  CHECK(broker.Execute({{"op", "status"}})["queue"].size() == 3);
  // No Chrome, no ownership error just to check whether the browser is busy.
  CHECK(!broker.Execute({{"op", "agent_status"}, {"session_id", b}})
             .contains("error"));
  CHECK(broker
            .Execute({{"op", "acquire"},
                      {"session_id", "bad/id"},
                      {"request_id", "1111111111111111"}})
            .contains("error"));
  CHECK(broker.Execute({{"op", "status"}, {"instance_id", "../"}})
            .contains("error"));
  broker.Execute({{"op", "cancel_acquire"},
                  {"session_id", b},
                  {"request_id", "1111111111111111"}});
  CHECK(acquire(c).value("position", 0) == 1);
  broker.Execute({{"op", "release"}, {"session_id", a}});
  // A newcomer cannot jump the existing queue, even before its head polls.
  CHECK(!acquire(b).value("acquired", false));
  auto second = acquire(c);
  CHECK(second.value("acquired", false));
  CHECK(second["lease"] != first["lease"]);
  CHECK(broker
            .Execute({{"op", "open"},
                      {"session_id", a},
                      {"lease", first["lease"]},
                      {"url", "https://example.com/"}})
            .contains("error"));
  // Releasing a former owner cannot release somebody else's browser.
  broker.Execute({{"op", "release"}, {"session_id", a}});
  CHECK(broker.Execute({{"op", "status"}}).value("session_id", "") == c);
  broker.Execute({{"op", "release"}, {"session_id", c}});
  broker.Execute({{"op", "cancel_acquire"},
                  {"session_id", b},
                  {"request_id", "1111111111111111"}});
  // A disconnected head expires. A polling waiter behind it stays queued.
  now += std::chrono::seconds(14);
  CHECK(!acquire(b).value("acquired", false));
  now += std::chrono::seconds(2);
  CHECK(acquire(b).value("acquired", false));
  // A forgotten owner cannot reserve the browser forever.
  now += std::chrono::minutes(3);
  CHECK(acquire(a).value("acquired", false));
  broker.Execute({{"op", "release"}, {"session_id", a}});
  // A published runtime disappearing releases its grant immediately.
  CreatePrivateDirectories(RuntimeDir());
  FileLease running;
  std::string error;
  CHECK(running.Acquire(session::SocketPathForId(a) + ".lock", error, true));
  CHECK(acquire(a).value("acquired", false));
  CHECK(!acquire(b).value("acquired", false));
  running.Reset();
  CHECK(acquire(b).value("acquired", false));
  broker.Shutdown();
  CHECK(acquire(c).contains("error"));
}

void TestBrowserParallelScheduling() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-parallel");
  const auto directory =
      fs::canonical("/tmp") / ("uagent-bp-" + std::to_string(getpid()));
  FakeBrowser fake(workspace.root / "bin", directory);
  CHECK(browser::EnsureDataDirectory(directory.string()));
  browser::Broker broker(directory.string());
  CHECK(broker.Execute({{"op", "configure"}, {"parallel_capacity", 5}})
            .contains("error"));
  CHECK(!broker.Execute({{"op", "configure"}, {"parallel_capacity", 2}})
             .contains("error"));
  const auto acquire = [&](const char* id, const char* kind = "auto") {
    return broker.Execute({{"op", "acquire"},
                           {"session_id", id},
                           {"request_id", "1111111111111111"},
                           {"browser", kind}});
  };
  constexpr const char* a = "aaaaaaaaaaaaaaaa";
  constexpr const char* b = "bbbbbbbbbbbbbbbb";
  constexpr const char* c = "cccccccccccccccc";
  auto one = acquire(a), two = acquire(b);
  CHECK(one.value("instance_id", "") == "1");
  CHECK(two.value("instance_id", "") == "2");
  CHECK(!acquire(c).value("acquired", false));
  auto shared = acquire("dddddddddddddddd", "shared");
  CHECK(shared.value("instance_id", "") == "0");
  const auto open = [&](const char* id, const json& grant) {
    return broker.Execute({{"op", "open"},
                           {"session_id", id},
                           {"lease", grant["lease"]},
                           {"url", "https://example.com/"}});
  };
  auto left = std::async(std::launch::async, [&] { return open(a, one); });
  auto right = std::async(std::launch::async, [&] { return open(b, two); });
  CHECK(!left.get().contains("error"));
  CHECK(!right.get().contains("error"));
  CHECK(fs::exists(directory / "p1/profile/cdp.jsonl"));
  CHECK(fs::exists(directory / "p2/profile/cdp.jsonl"));
  // A slow page must not queue viewer heartbeat threads behind its runtime
  // lock, or prevent another research instance from progressing.
  std::ofstream(directory / "p1/profile/pause-navigation").close();
  auto paused = std::async(std::launch::async, [&] { return open(a, one); });
  for (int wait = 0;
       wait < 100 && !fs::exists(directory / "p1/profile/navigation-paused");
       ++wait)
    poll(nullptr, 0, 10);
  CHECK(fs::exists(directory / "p1/profile/navigation-paused"));
  for (int heartbeat = 0; heartbeat < 20; ++heartbeat) {
    auto viewer = broker.Execute({{"op", "viewer"}, {"instance_id", "1"}});
    CHECK(viewer.value("ok", false));
    CHECK(!viewer.value("controller", true));
  }
  CHECK(!open(b, two).contains("error"));
  CHECK(paused.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready);
  fs::remove(directory / "p1/profile/pause-navigation");
  CHECK(!paused.get().contains("error"));
  // Human control of one instance neither claims nor blocks the other.
  constexpr const char* device = "eeeeeeeeeeeeeeee";
  auto taken = broker.Execute(
      {{"op", "takeover"}, {"device", device}, {"instance_id", "1"}});
  CHECK(taken.value("mode", "") == "human");
  CHECK(
      broker
          .Execute({{"op", "viewer"}, {"device", device}, {"instance_id", "1"}})
          .value("controller", false));
  CHECK(!open(b, two).contains("error"));
  // A turn ending during human control defers release until handback.
  broker.Execute({{"op", "release"}, {"session_id", a}});
  CHECK(!acquire(c).value("acquired", false));
  CHECK(!broker
             .Execute({{"op", "done"},
                       {"device", device},
                       {"instance_id", "1"},
                       {"interaction_id", ""}})
             .contains("error"));
  auto next = acquire(c);
  CHECK(next.value("acquired", false));
  CHECK(next.value("instance_id", "") == "1");
  std::ofstream(directory / "p1/profile/private-cookie") << "previous owner";
  CHECK(!open(c, next).contains("error"));
  CHECK(!fs::exists(directory / "p1/profile/private-cookie"));
  CHECK(broker
            .Execute({{"op", "click"},
                      {"session_id", c},
                      {"lease", one["lease"]},
                      {"element", 1},
                      {"view_id", "old"}})
            .contains("error"));
  // Capacity survives service restart; isolated browsing never touches the
  // shared persistent profile.
  CHECK(!fs::exists(directory / "profile"));
  broker.Shutdown();
  browser::Broker restored(directory.string());
  CHECK(restored.Execute({{"op", "status"}}).value("parallel_capacity", 0) ==
        2);
  restored.Shutdown();
  fs::remove_all(directory);
}

void TestBrowserProfileSignIn() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-signin");
  const auto directory = fs::canonical(workspace.root) / "browser";
  const auto bin = workspace.root / "bin";
  FakeBrowser fake(bin, directory);
  constexpr const char* kDevice = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr const char* kOther = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  browser::Runtime runtime;
  CHECK(runtime.Execute({{"op", "setup_profile"}, {"device", kDevice}})
            .contains("error"));
  // Another browser service on the machine holds display :99.
  const bool took_display = !fs::exists("/tmp/.X99-lock");
  if (took_display) {
    std::ofstream("/tmp/.X99-lock");
  }
  CHECK(runtime.Execute({{"op", "takeover"}, {"device", kDevice}})
            .value("running", false));
  if (took_display) {
    fs::remove("/tmp/.X99-lock");
  }
  // Each Chrome gets its own display environment; the broker's process
  // environment stays untouched when another worker starts.
  CHECK(getenv("DISPLAY") == nullptr);
  std::ifstream launches(directory / "profile" / "launches.jsonl");
  std::string launch;
  CHECK(static_cast<bool>(std::getline(launches, launch)));
  CHECK(json::parse(launch).value("display", "") != ":99");
  CHECK(runtime.Execute({{"op", "setup_profile"}, {"device", kOther}})
            .contains("error"));
  const auto setup =
      runtime.Execute({{"op", "setup_profile"}, {"device", kDevice}});
  CHECK(setup.value("profile_setup", false));
  CHECK(setup.value("viewer", "") == kDevice);
  CHECK(!setup.contains("url"));
  const auto profile = directory / "profile";
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(BudgetMs(3000));
  while (!fs::exists(profile / "manual-ready") &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(fs::exists(profile / "manual-ready"));
  CHECK(runtime.Execute({{"op", "tabs"}, {"session_id", kOther}})
            .contains("error"));
  CHECK(runtime.Execute({{"op", "takeover"}, {"device", kDevice}})
            .value("profile_setup", false));
  const auto prepared =
      runtime.Execute({{"op", "prepare_done"}, {"device", kDevice}});
  CHECK(!prepared.contains("error"));
  CHECK(!prepared.value("profile_setup", true));
  CHECK(prepared.value("mode", "") == "human");
  CHECK(fs::exists(profile / "saved-login"));
  std::ifstream log(profile / "launches.jsonl");
  std::vector<bool> controlled;
  for (std::string line; std::getline(log, line);) {
    controlled.push_back(json::parse(line).value("controlled", false));
  }
  CHECK(controlled == std::vector<bool>({true, false, true}));
  CHECK(runtime.Execute({{"op", "done"}, {"device", kDevice}})
            .value("mode", "") == "idle");
}

// Closing the controlling viewer hands the browser back, watching never takes
// control, and an agent call made meanwhile fails at once instead of waiting.
// An idle browser stops only while no human holds it.
void TestBrowserHandBackOnClose() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-handback");
  const auto bin = workspace.root / "bin";
  FakeBrowser fake(bin, fs::canonical(workspace.root) / "browser");
  constexpr const char* kDevice = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr const char* kSession = "cccccccccccccccccccccccccccccccc";
  browser::Runtime runtime;
  auto taken = runtime.Execute({{"op", "takeover"}, {"device", kDevice}});
  CHECK(taken.value("mode", "") == "human");
  CHECK(runtime.Execute({{"op", "viewer"}, {"device", kDevice}})
            .value("controller", false));
  CHECK(!runtime.Execute({{"op", "viewer"}, {"device", kSession}})
             .value("controller", true));
  CHECK(runtime.Execute({{"op", "tabs"}, {"session_id", kSession}})
            .value("error", "")
            .starts_with("the user controls the browser"));
  CHECK(!runtime.Execute({{"op", "status"}}).value("waiting", true));
  // A stale viewer of an earlier display cannot hand back this one.
  CHECK(runtime
            .Execute({{"op", "viewer_disconnected"},
                      {"device", kDevice},
                      {"display", taken.value("display", uint64_t{0}) + 1},
                      {"generation", taken["generation"]}})
            .value("mode", "") == "human");
  // Nor can one that last saw an earlier lease of this display.
  CHECK(
      runtime
          .Execute({{"op", "viewer_disconnected"},
                    {"device", kDevice},
                    {"display", taken["display"]},
                    {"generation", taken.value("generation", uint64_t{0}) - 1}})
          .value("mode", "") == "human");
  auto closed = runtime.Execute({{"op", "viewer_disconnected"},
                                 {"device", kDevice},
                                 {"display", taken["display"]},
                                 {"generation", taken["generation"]}});
  CHECK(closed.value("mode", "") == "idle");
  CHECK(!closed.value("waiting", true));
  CHECK(!runtime.Execute({{"op", "tabs"}, {"session_id", kSession}})
             .contains("error"));
  taken = runtime.Execute({{"op", "takeover"}, {"device", kDevice}});
  CHECK(taken.value("mode", "") == "human");
  closed = runtime.Execute({{"op", "viewer_disconnected"},
                            {"device", kDevice},
                            {"display", taken["display"]},
                            {"generation", taken["generation"]}});
  CHECK(closed.value("mode", "") == "agent");
  // An idle Chrome stops but keeps the agent's lease, and the next action
  // starts it again; one a human controls never stops.
  runtime.StopIfIdle(std::chrono::minutes(0));
  CHECK(!runtime.Execute({{"op", "status"}}).value("running", true));
  CHECK(runtime.Execute({{"op", "status"}}).value("mode", "") == "agent");
  CHECK(!runtime.Execute({{"op", "tabs"}, {"session_id", kSession}})
             .contains("error"));
  // The end of a turn leaves only its current tab open.
  CHECK(!runtime.Execute({{"op", "release"}, {"session_id", kSession}})
             .contains("error"));
  std::vector<std::string> closed_tabs;
  std::ifstream cdp(fs::canonical(workspace.root) / "browser" / "profile" /
                    "cdp.jsonl");
  for (std::string line; std::getline(cdp, line);) {
    const json command = json::parse(line);
    if (command.value("method", "") == "Target.closeTarget") {
      closed_tabs.push_back(command["params"].value("targetId", ""));
    }
  }
  CHECK(closed_tabs == std::vector<std::string>({"left-open"}));
  CHECK(runtime.Execute({{"op", "takeover"}, {"device", kDevice}})
            .value("running", false));
  runtime.StopIfIdle(std::chrono::minutes(0));
  CHECK(runtime.Execute({{"op", "status"}}).value("running", false));
  runtime.Shutdown();
}

// Every attached page masks password fields before anything is drawn, and
// back refuses a history entry the agent could not open by URL.
void TestBrowserSecretMaskAndBack() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-secrets");
  const auto bin = workspace.root / "bin";
  // Short, so the display's socket fits an address on every platform.
  const auto directory =
      fs::canonical("/tmp") / ("uagent-browser-" + std::to_string(getpid()));
  FakeBrowser fake(bin, directory);
  constexpr const char* kSession = "cccccccccccccccccccccccccccccccc";
  browser::Runtime runtime;
  // Whoever takes the browser looks before acting on it.
  CHECK(runtime.Execute({{"op", "back"}, {"session_id", kSession}})
            .value("error", "")
            .starts_with("another conversation used the browser since"));
  CHECK(!runtime.Execute({{"op", "tabs"}, {"session_id", kSession}})
             .contains("error"));
  CHECK(runtime.Execute({{"op", "back"}, {"session_id", kSession}})
            .value("error", "") == "the previous page is not HTTP(S)");
  bool masked = false, navigated = false;
  std::ifstream log(directory / "profile" / "cdp.jsonl");
  for (std::string line; std::getline(log, line);) {
    const json command = json::parse(line);
    const std::string method = command.value("method", "");
    if (method == "Page.addScriptToEvaluateOnNewDocument") {
      masked =
          command["params"].value("runImmediately", false) &&
          command["params"].value("source", "").find("-webkit-text-security") !=
              std::string::npos;
    }
    navigated = navigated || method == "Page.navigateToHistoryEntry";
  }
  CHECK(masked);
  CHECK(!navigated);
  // A tab closed by hand leaves nothing attached: the next action attaches
  // a page again instead of failing on the one that is gone.
  std::ofstream(directory / "profile" / "close-tab").close();
  CHECK(runtime.Execute({{"op", "back"}, {"session_id", kSession}})
            .value("error", "") == "the previous page is not HTTP(S)");
  // A key reaches Chrome whole, with its code and what it types: by name
  // alone Enter sends no form. replace selects what the field holds first.
  // A new tab leaves the open page alone, and a tab that is not the current
  // one can be closed.
  const auto act = [&](json command) {
    command["session_id"] = kSession;
    return runtime.Execute(command);
  };
  CHECK(!act({{"op", "press"}, {"key", "Enter"}}).contains("error"));
  CHECK(act({{"op", "press"}, {"key", "F5"}}).value("error", "") ==
        "unsupported key");
  CHECK(!act({{"op", "type"}, {"text", "new"}, {"replace", true}})
             .contains("error"));
  CHECK(
      !act({{"op", "open"}, {"url", "https://example.com/"}, {"new_tab", true}})
           .contains("error"));
  CHECK(act({{"op", "tabs"}, {"target_id", "left-open"}, {"close", true}})
            .value("closed", "") == "left-open");
  // A look without its picture takes no screenshot, and what it numbered
  // can be clicked by number: where that element is now is where the mouse
  // goes. A number the page no longer has is refused, not guessed.
  json looked = act({{"op", "observe"}, {"image", false}});
  CHECK(looked.value("image", "x").empty());
  const std::string view = looked.value("view_id", "");
  CHECK(act({{"op", "click"}, {"element", 2}, {"view_id", view}})
            .value("error", "") == "that element is gone; look again");
  CHECK(!act({{"op", "click"}, {"element", 1}, {"view_id", view}})
             .contains("error"));
  // An earlier look at the same page still names its elements, so one look
  // serves several actions; a point needs the latest look, and another page
  // takes none of the earlier ones.
  CHECK(act({{"op", "observe"}, {"image", false}}).contains("view_id"));
  CHECK(!act({{"op", "click"}, {"element", 1}, {"view_id", view}})
             .contains("error"));
  CHECK(act({{"op", "observe"}, {"image", false}}).contains("view_id"));
  CHECK(act({{"op", "click"}, {"x", 5}, {"y", 5}, {"view_id", view}})
            .value("error", "") ==
        "click needs view_id from the latest observe");
  CHECK(!act({{"op", "open"}, {"url", "https://example.com/"}})
             .contains("error"));
  CHECK(act({{"op", "observe"}, {"image", false}}).contains("view_id"));
  CHECK(act({{"op", "click"}, {"element", 1}, {"view_id", view}})
            .value("error", "") ==
        "click needs view_id from a look at this page");
  CHECK(act({{"op", "observe"}}).value("image", "") == "c2hvdA==");
  CHECK(act({{"op", "read"}, {"find", "clip"}}).value("text", "") ==
        "Clip one");
  bool pictured = false, pointed = false;
  bool entered = false, selected = false, created = false, closed = false;
  std::ifstream acted(directory / "profile" / "cdp.jsonl");
  for (std::string line; std::getline(acted, line);) {
    const json command = json::parse(line);
    const std::string method = command.value("method", "");
    const json params = command.value("params", json::object());
    entered = entered || (params.value("type", "") == "keyDown" &&
                          params.value("windowsVirtualKeyCode", 0) == 13 &&
                          params.value("text", "") == "\r");
    selected = selected || params.value("commands", json::array()) ==
                               json::array({"selectAll"});
    // The tab is made before the page is asked for.
    created = created || method == "Target.createTarget";
    if (method == "Page.navigate") CHECK(created);
    closed = closed || (method == "Target.closeTarget" &&
                        params.value("targetId", "") == "left-open");
    // The one screenshot comes after the click: the first look took none.
    pointed =
        pointed || (params.value("type", "") == "mousePressed" &&
                    params.value("x", 0) == 40 && params.value("y", 0) == 30);
    if (method == "Page.captureScreenshot") {
      CHECK(pointed && !pictured);
      pictured = true;
    }
  }
  CHECK(entered && selected && created && closed && pointed && pictured);
  // A saved login is taken with keys on the display itself, where Chrome's
  // own list listens: Escape, Down, Down, Tab, each pressed and let go. A
  // field that stays as long as it was had nothing saved for it.
  CHECK(runtime.Execute({{"op", "fill_saved"}, {"session_id", kSession}})
            .value("error", "")
            .starts_with("Chrome filled nothing"));
  std::ofstream(directory / "profile" / "saved-login-here").close();
  fs::remove(directory / "display.sock.keys");
  CHECK(!runtime.Execute({{"op", "fill_saved"}, {"session_id", kSession}})
             .contains("error"));
  std::ifstream keys(directory / "display.sock.keys");
  const std::string pressed((std::istreambuf_iterator<char>(keys)),
                            std::istreambuf_iterator<char>());
  CHECK(pressed ==
        "1 ff1b\n0 ff1b\n1 ff54\n0 ff54\n1 ff54\n0 ff54\n"
        "1 ff09\n0 ff09\n");
  // One browser, one conversation at a time: another waits for the turn of
  // the one that holds it to end.
  constexpr const char* kOther = "dddddddddddddddddddddddddddddddd";
  CHECK(runtime.Execute({{"op", "tabs"}, {"session_id", kOther}})
            .value("error", "")
            .starts_with("another conversation is using the browser"));
  CHECK(!runtime.Execute({{"op", "release"}, {"session_id", kSession}})
             .contains("error"));
  CHECK(!runtime.Execute({{"op", "tabs"}, {"session_id", kOther}})
             .contains("error"));
  runtime.Shutdown();
  fs::remove_all(directory);
}

// A page's dialog holds the page until it is answered: the click that opened
// it returns at once, a look reports it, even after a visit to another tab,
// and only Enter or Escape (with text typed first for a prompt) reach it. A
// plea to stay is accepted unasked only when the agent is leaving the page.
// A password field takes no typed text, nor does another site's frame.
void TestBrowserDialogsAndPasswordFields() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-dialogs");
  const auto directory =
      fs::canonical("/tmp") / ("uagent-dialog-" + std::to_string(getpid()));
  FakeBrowser fake(workspace.root / "bin", directory);
  const auto profile = directory / "profile";
  browser::Runtime runtime;
  const auto act = [&](json command) {
    command["session_id"] = "cccccccccccccccccccccccccccccccc";
    return runtime.Execute(command);
  };
  const auto click = [&] {
    const std::string view =
        act({{"op", "observe"}, {"image", false}}).value("view_id", "");
    return act({{"op", "click"}, {"element", 1}, {"view_id", view}});
  };
  CHECK(!act({{"op", "tabs"}}).contains("error"));
  std::ofstream(profile / "dialog-on-click") << "confirm";
  const auto started = std::chrono::steady_clock::now();
  CHECK(!click().contains("error"));
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
  const json shown = act({{"op", "observe"}});
  CHECK(
      shown.value("dialog", json::object()) ==
      json({{"type", "confirm"}, {"message", "Really confirm?"}, {"url", ""}}));
  CHECK(act({{"op", "back"}})
            .value("error", "")
            .starts_with("a confirm dialog is open"));
  CHECK(!act({{"op", "tabs"}, {"target_id", "left-open"}}).contains("error"));
  CHECK(act({{"op", "observe"}}).contains("view_id"));
  CHECK(!act({{"op", "tabs"}, {"target_id", "tab"}}).contains("error"));
  CHECK(act({{"op", "observe"}}).contains("dialog"));
  CHECK(!act({{"op", "press"}, {"key", "Escape"}}).contains("error"));
  CHECK(act({{"op", "observe"}}).contains("view_id"));
  std::ofstream(profile / "dialog-on-click") << "prompt";
  CHECK(!click().contains("error"));
  CHECK(!act({{"op", "type"}, {"text", "Ada"}}).contains("error"));
  CHECK(!act({{"op", "press"}, {"key", "Enter"}}).contains("error"));
  std::ofstream(profile / "dialog-on-click") << "beforeunload";
  CHECK(!click().contains("error"));
  CHECK(act({{"op", "observe"}}).contains("dialog"));
  CHECK(!act({{"op", "press"}, {"key", "Enter"}}).contains("error"));
  std::ofstream(profile / "dialog-on-navigate") << "beforeunload";
  CHECK(!act({{"op", "open"}, {"url", "https://example.com/"}})
             .contains("error"));
  CHECK(act({{"op", "observe"}}).contains("view_id"));
  // A dialog on the press, or while the page is looked at, is reported in
  // place of what was asked, never as a failure of it.
  std::ofstream(profile / "dialog-on-press") << "confirm";
  CHECK(!click().contains("error"));
  CHECK(act({{"op", "observe"}}).contains("dialog"));
  CHECK(!act({{"op", "press"}, {"key", "Escape"}}).contains("error"));
  std::ofstream(profile / "dialog-on-look") << "alert";
  CHECK(act({{"op", "observe"}}).value("dialog", json()).value("type", "") ==
        "alert");
  CHECK(!act({{"op", "press"}, {"key", "Enter"}}).contains("error"));
  // On a page that is no web site, a call runs on the look it was approved
  // on, not after a newer one.
  CHECK(act({{"op", "observe"}}).contains("view_id"));
  CHECK(act({{"op", "press"}, {"key", "Tab"}, {"look", "older"}})
            .value("error", "")
            .starts_with("the page changed since the look"));
  std::ofstream(profile / "password-focused").close();
  CHECK(act({{"op", "type"}, {"text", "hunter2"}}).value("error", "") ==
        "this is a password field: use fill_saved, or request_human");
  std::ofstream(profile / "password-focused") << "frame";
  CHECK(act({{"op", "type"}, {"text", "hunter2"}})
            .value("error", "")
            .starts_with("the field is in another site's frame"));
  std::vector<json> answers;
  int attached = 0;
  bool moved = false, typed = false;
  std::ifstream log(profile / "cdp.jsonl");
  for (std::string line; std::getline(log, line);) {
    const json command = json::parse(line);
    const std::string method = command.value("method", "");
    const json params = command.value("params", json::object());
    if (method == "Page.handleJavaScriptDialog") answers.push_back(params);
    attached += method == "Target.attachToTarget" &&
                params.value("targetId", "") == "tab";
    moved = moved || params.value("type", "") == "mouseMoved";
    if (params.value("type", "") == "mousePressed") CHECK(moved);
    typed = typed || params.value("text", "") == "hunter2";
  }
  CHECK(answers == std::vector<json>({{{"accept", false}},
                                      {{"accept", true}, {"promptText", "Ada"}},
                                      {{"accept", true}},
                                      {{"accept", true}},
                                      {{"accept", false}},
                                      {{"accept", true}}}));
  CHECK(attached == 1);
  CHECK(!typed);
  runtime.Shutdown();
  fs::remove_all(directory);
}

// A file field takes a file; a button's chooser is held back and its field
// takes it; a click that opens a chooser says so; what a page downloads lands
// in the folder the action names and is told when it ends.
void TestBrowserUploadsAndDownloads() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-files");
  const auto directory =
      fs::canonical("/tmp") / ("uagent-files-" + std::to_string(getpid()));
  FakeBrowser fake(workspace.root / "bin", directory);
  const auto profile = directory / "profile";
  browser::Runtime runtime;
  const auto act = [&](json command) {
    command["session_id"] = "cccccccccccccccccccccccccccccccc";
    return runtime.Execute(command);
  };
  const auto view = [&] {
    return act({{"op", "observe"}, {"image", false}}).value("view_id", "");
  };
  CHECK(!act({{"op", "tabs"}}).contains("error"));
  std::ofstream(profile / "file-input").close();
  CHECK(!act({{"op", "upload"},
              {"element", 1},
              {"view_id", view()},
              {"file", "/work/report.pdf"}})
             .contains("error"));
  fs::remove(profile / "file-input");
  std::ofstream(profile / "chooser-on-click").close();
  CHECK(!act({{"op", "upload"},
              {"element", 1},
              {"view_id", view()},
              {"file", "/work/photo.png"}})
             .contains("error"));
  CHECK(act({{"op", "click"}, {"element", 1}, {"view_id", view()}})
            .value("chooser", false));
  fs::remove(profile / "chooser-on-click");
  CHECK(act({{"op", "upload"},
             {"element", 1},
             {"view_id", view()},
             {"file", "/work/photo.png"}})
            .value("error", "")
            .starts_with("element 1 opened no file chooser"));
  std::ofstream(profile / "download-on-click").close();
  CHECK(!act({{"op", "click"},
              {"element", 1},
              {"view_id", view()},
              {"downloads", "/work/downloads"}})
             .contains("error"));
  CHECK(act({{"op", "probe"}}).value("downloads", json()) ==
        json::array({{{"name", "report.pdf"}, {"state", "completed"}}}));
  std::vector<json> files;
  std::string into;
  std::ifstream log(profile / "cdp.jsonl");
  for (std::string line; std::getline(log, line);) {
    const json command = json::parse(line);
    const std::string method = command.value("method", "");
    const json params = command.value("params", json::object());
    if (method == "DOM.setFileInputFiles") files.push_back(params);
    if (method == "Browser.setDownloadBehavior") {
      into = params.value("downloadPath", "");
    }
  }
  CHECK(files.size() == 2);
  CHECK(files[0]["files"] == json::array({"/work/report.pdf"}) &&
        files[0].contains("objectId"));
  CHECK(files[1]["files"] == json::array({"/work/photo.png"}) &&
        files[1].value("backendNodeId", 0) == 7);
  CHECK(into == "/work/downloads");
  runtime.Shutdown();
  fs::remove_all(directory);
}

// The site an approval covers: scheme and host as a browser compares them.
void TestBrowserOrigin() {
  CHECK(browser::Origin("https://Example.COM/a?b#c") == "https://example.com");
  CHECK(browser::Origin("https://user:pw@example.com:443/") ==
        "https://example.com");
  CHECK(browser::Origin("http://example.com:8080?x") ==
        "http://example.com:8080");
  CHECK(browser::Origin("https://[::1]:8443/") == "https://[::1]:8443");
  CHECK(browser::Origin("about:blank").empty());
  // Read as a browser reads it: a backslash ends the host. What a browser
  // might read otherwise covers nothing.
  CHECK(browser::Origin("https://evil.example\\@good.example/") ==
        "https://evil.example");
  CHECK(browser::Origin("https://good%2eexample/").empty());
  CHECK(browser::Origin("https://good.example:0443/").empty());
  CHECK(browser::Origin("https://[]/").empty());
  CHECK(browser::Origin("https://good.example:70000/").empty());
  CHECK(browser::Origin("file:///etc/hosts").empty());
}

// The tool's own part: several steps in one call, in order, and a page that
// is only read. The service runs here, in front of the stand-in Chrome.
void TestBrowserToolQueue() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-tool-queue");
  const auto directory =
      fs::canonical("/tmp") / ("uagent-bq-" + std::to_string(getpid()));
  FakeBrowser fake(workspace.root / "bin", directory);
  int owner[2];
  CHECK(pipe(owner) == 0);
  CHECK(fcntl(owner[1], F_SETFD, FD_CLOEXEC) == 0);
  std::thread service([&] { browser::ServiceMain(owner[0]); });
  for (int wait = 0; wait < 500 && !fs::exists(browser::SocketPath()); ++wait)
    poll(nullptr, 0, 10);
  constexpr const char* a = "aaaaaaaaaaaaaaaa";
  constexpr const char* b = "bbbbbbbbbbbbbbbb";
  const auto acquire = [&] {
    return browser::Request({{"op", "acquire"},
                             {"session_id", a},
                             {"request_id", "1111111111111111"}});
  };
  CHECK(acquire().value("acquired", false));
  const Tool tool = BrowserTool(b, [](const std::string&, const std::string&,
                                      bool*) { return std::string(); });
  ToolContext timed(std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(100));
  CHECK(!tool.run({{"action", "open"}, {"url", "https://example.com/"}}, timed)
             .Ok());
  CHECK(browser::Request({{"op", "status"}})["queue"].empty());
  CHECK(!fs::exists(directory / "profile"));
  std::atomic<bool> abort{false};
  auto cancelled = std::async(std::launch::async, [&] {
    LocalAbort local(abort);
    return tool.run({{"action", "open"}, {"url", "https://example.com/"}}, {});
  });
  for (int wait = 0; wait < 100; ++wait) {
    if (!browser::Request({{"op", "status"}})["queue"].empty()) break;
    poll(nullptr, 0, 10);
  }
  CHECK(!browser::Request({{"op", "status"}})["queue"].empty());
  abort = true;
  CHECK(!cancelled.get().Ok());
  CHECK(browser::Request({{"op", "status"}})["queue"].empty());
  CHECK(!fs::exists(directory / "profile"));
  // The tool waits below the model loop: one call eventually performs one
  // navigation, with an observable queued state and no retry by the model.
  ToolContext context(std::chrono::steady_clock::now() +
                      std::chrono::seconds(10));
  std::vector<std::string> labels;
  context.progress = [&](const std::string& label) { labels.push_back(label); };
  auto pending = std::async(std::launch::async, [&] {
    return tool.run({{"action", "open"}, {"url", "https://example.com/"}},
                    context);
  });
  for (int wait = 0; wait < 100; ++wait) {
    if (!browser::Request({{"op", "status"}})["queue"].empty()) break;
    poll(nullptr, 0, 10);
  }
  CHECK(!browser::Request({{"op", "status"}})["queue"].empty());
  CHECK(pending.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready);
  CHECK(!browser::Request({{"op", "release"}, {"session_id", a}})
             .contains("error"));
  CHECK(pending.get().Ok());
  CHECK(labels.size() >= 2);
  CHECK(labels.front().starts_with("Waiting for browser"));
  CHECK(labels.back() == "Using browser");
  int navigations = 0;
  std::ifstream log(directory / "profile/cdp.jsonl");
  for (std::string line; std::getline(log, line);) {
    if (json::parse(line).value("method", "") == "Page.navigate") ++navigations;
  }
  CHECK(navigations == 1);
  CHECK(browser::Request({{"op", "status"}})["queue"].empty());
  close(owner[1]);
  service.join();
  close(owner[0]);
  fs::remove_all(directory);
}

void TestBrowserToolSteps() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-tool");
  const auto bin = workspace.root / "bin";
  const auto directory = fs::canonical("/tmp") /
                         ("uagent-browser-tool-" + std::to_string(getpid()));
  FakeBrowser fake(bin, directory);
  int owner[2];
  CHECK(pipe(owner) == 0);
  // Chrome must not inherit the end whose closing stops the service.
  CHECK(fcntl(owner[1], F_SETFD, FD_CLOEXEC) == 0);
  std::thread service([&] { browser::ServiceMain(owner[0]); });
  for (int wait = 0; wait < 500 && !fs::exists(browser::SocketPath()); ++wait) {
    poll(nullptr, 0, 10);
  }
  const Tool tool = BrowserTool("cccccccccccccccccccccccccccccccc",
                                [](const std::string&, const std::string&,
                                   bool*) { return std::string(); });
  const auto view = [](const std::string& output) {
    const size_t at = output.find("View ID: ") + 9;
    return output.substr(at, output.find(' ', at) - at);
  };
  // An action answers with the page as text, and no picture.
  ToolResult opened =
      tool.run({{"action", "open"}, {"url", "https://example.com/"}}, {});
  CHECK(opened.Ok());
  CHECK(opened.output.starts_with("open done.\nURL: https://example.com/"));
  CHECK(opened.output.find("Elements:\n[1] text \"Search\" (empty)") !=
        std::string::npos);
  CHECK(opened.output.find("image saved") == std::string::npos);
  // An approval names what an action acts on and where, and a remembered
  // one covers that action on the site, whatever the look it came from.
  const json click = {
      {"action", "click"}, {"element", 1}, {"view_id", view(opened.output)}};
  CHECK(tool.approval_preview(click) ==
        "click [1] text \"Search\" (empty) on https://example.com/\n"
        "Remembered, it covers any browser click on https://example.com.");
  CHECK(tool.approval_preview({{"action", "type"},
                               {"element", 1},
                               {"view_id", view(opened.output)},
                               {"text", "kestrel"},
                               {"key", "Enter"}}) ==
        "type \"kestrel\" into [1] text \"Search\" (empty) then press Enter "
        "on https://example.com/\nRemembered, it covers any browser type on "
        "https://example.com.");
  // A row of another look is not named as this page's.
  CHECK(tool.approval_preview(
                {{"action", "click"}, {"element", 1}, {"view_id", "older"}})
            .starts_with("click element [1] of a look that is not the latest"));
  // open takes one address; a tab is approved one by one.
  CHECK(tool.validate({{"action", "open"}}).has_value());
  CHECK(tool.validate({{"action", "open"},
                       {"url", "https://example.com/"},
                       {"path", "page.html"}})
            .has_value());
  CHECK(tool.approval_subject({{"action", "tabs"}, {"target_id", "a"}}) !=
        tool.approval_subject({{"action", "tabs"}, {"target_id", "b"}}));

  CHECK(
      tool.approval_subject(click) ==
      tool.approval_subject(
          {{"action", "click"}, {"element", 3}, {"view_id", "another-look"}}));
  CHECK(tool.approval_subject(click) !=
        tool.approval_subject({{"action", "type"}, {"text", "x"}}));
  CHECK(tool.approval_subject(
            {{"action", "open"}, {"url", "https://example.com/a?b"}}) ==
        tool.approval_subject(
            {{"action", "open"}, {"url", "https://example.com/c"}}));
  // A field in one call: clicked, its content replaced, the key pressed.
  ToolResult filled = tool.run({{"action", "type"},
                                {"element", 1},
                                {"view_id", view(opened.output)},
                                {"text", "kestrel"},
                                {"replace", true},
                                {"key", "Enter"}},
                               {});
  CHECK(filled.output.starts_with("click done.\ntype done.\npress done.\n"));
  // A field that is gone stops the call before anything is typed.
  CHECK(!tool.run({{"action", "type"},
                   {"element", 2},
                   {"view_id", view(filled.output)},
                   {"text", "lost"},
                   {"key", "Enter"}},
                  {})
             .Ok());
  // A click that opens a dialog is done, and the look after it is the
  // dialog: the page behind it cannot be read until it is answered.
  std::ofstream(directory / "profile" / "dialog-on-click") << "confirm";
  ToolResult asked = tool.run(
      {{"action", "click"}, {"element", 1}, {"view_id", view(filled.output)}},
      {});
  CHECK(asked.output ==
        "click done.\nA confirm dialog is open: \"Really "
        "confirm?\". press Enter accepts it, Escape dismisses "
        "it.");
  ToolResult answered = tool.run({{"action", "press"}, {"key", "Enter"}}, {});
  CHECK(answered.Ok());
  // A field whose click opens a prompt keeps what was meant for it: the call
  // stops at the dialog, and nothing is typed or pressed into it.
  std::ofstream(directory / "profile" / "dialog-on-click") << "prompt";
  ToolResult stopped = tool.run({{"action", "type"},
                                 {"element", 1},
                                 {"view_id", view(answered.output)},
                                 {"text", "meant for the page"},
                                 {"key", "Enter"}},
                                {});
  CHECK(stopped.output.starts_with("click done.\nA prompt dialog is open"));
  // Nothing was typed into the prompt that type's own click opened.
  CHECK(tool.approval_preview({{"action", "press"}, {"key", "Enter"}})
            .find("its answer reads") == std::string::npos);
  // Approving the Enter that sends a prompt's answer shows that answer.
  CHECK(tool.run({{"action", "type"}, {"text", "final"}}, {}).Ok());
  CHECK(tool.approval_preview({{"action", "press"}, {"key", "Enter"}})
            .find("while a prompt dialog says \"Really prompt?\" and its "
                  "answer reads \"final\"") != std::string::npos);
  CHECK(tool.run({{"action", "press"}, {"key", "Escape"}}, {}).Ok());
  // A page that is only read: opened, then the lines asked for.
  ToolResult read = tool.run(
      {{"action", "open"}, {"url", "https://example.com/"}, {"find", "clip"}},
      {});
  CHECK(read.output.starts_with("open done.\nURL: https://example.com/"));
  CHECK(read.output.ends_with("Lines containing it (1 in all):\nClip one"));
  CHECK(read.output.find("Elements:") == std::string::npos);
  // A page the agent wrote opens from its file with no server: its markup
  // is written into a blank page. A file:// URL stays refused, and so does
  // any file that is not a page, or that lies in the browser's own profile.
  const fs::path page = workspace.root / "chart.html";
  std::ofstream(page) << "<p>CHART_MARKUP</p>";
  CHECK(tool.run({{"action", "open"}, {"path", page.string()}}, {}).Ok());
  CHECK(!tool.run({{"action", "open"}, {"url", "file://" + page.string()}}, {})
             .Ok());
  const fs::path notes = workspace.root / "notes.txt";
  std::ofstream(notes) << "plain";
  ToolResult plain =
      tool.run({{"action", "open"}, {"path", notes.string()}}, {});
  CHECK(!plain.Ok() && plain.output.find(".html or .svg") != std::string::npos);
  CHECK(tool.validate({{"action", "open"},
                       {"path", (directory / "profile" / "x.html").string()}})
            .has_value());
  // What a click downloads is told, in the conversation's folder; only a
  // file of that folder is uploaded.
  std::ofstream(directory / "profile" / "download-on-click").close();
  ToolResult again =
      tool.run({{"action", "open"}, {"url", "https://example.com/"}}, {});
  ToolResult fetched = tool.run(
      {{"action", "click"}, {"element", 1}, {"view_id", view(again.output)}},
      {});
  CHECK(fetched.output.find("Downloaded report.pdf to downloads/report.pdf.") !=
        std::string::npos);
  CHECK(tool.validate(
                {{"action", "upload"}, {"element", 1}, {"path", "/etc/hosts"}})
            .has_value());
  // A call runs on the site its approval was for: there it runs, and once
  // the tab is elsewhere (after a batch's open, or a person's visit) the
  // browser refuses it.
  const json press = {{"action", "press"}, {"key", "Tab"}};
  ToolContext approved;
  approved.approved = tool.approval_subject(press);
  CHECK(approved.approved.value("origin", "") == "https://example.com");
  CHECK(tool.run(press, approved).Ok());
  approved.approved["origin"] = "https://other.example";
  ToolResult held = tool.run(press, approved);
  CHECK(!held.Ok() &&
        held.output.find("where this was approved") != std::string::npos);
  close(owner[1]);
  service.join();
  close(owner[0]);
  std::vector<std::string> order;
  bool blank = false, written = false;
  std::ifstream log(directory / "profile" / "cdp.jsonl");
  for (std::string line; std::getline(log, line);) {
    const json params = json::parse(line).value("params", json::object());
    blank = blank || params.value("url", "") == "about:blank";
    if (params.value("html", "") == "<p>CHART_MARKUP</p>") {
      CHECK(blank);
      written = true;
    }
    CHECK(!params.value("url", "").starts_with("file:"));
    if (params.value("type", "") == "mousePressed") order.emplace_back("click");
    if (params.contains("commands")) order.emplace_back("select");
    if (params.contains("text") && !params.contains("key")) {
      order.push_back("type " + params.value("text", ""));
    }
    if (params.value("type", "") == "keyDown") order.emplace_back("Enter");
  }
  CHECK(order ==
        std::vector<std::string>({"click", "select", "type kestrel", "Enter",
                                  "click", "click", "click"}));
  CHECK(written);
  fs::remove_all(directory);
}

}  // namespace uagent
