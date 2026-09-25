// Copyright 2026 Timon Gentzsch

#include "include/browser/browser.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "include/browser/runtime.h"
#include "include/core/json.h"
#include "include/web/rfb_filter.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestBrowserViewOnlyFilter() {
  web::RfbViewOnlyFilter filter;
  std::string output;
  CHECK(filter.Push("RFB 003.", output));
  CHECK(output == "RFB 003.");
  CHECK(filter.Push(std::string("008\n\x01\x01", 6), output));
  CHECK(output == std::string("008\n\x01\x01", 6));

  const std::string update("\x03\x01\0\0\0\0\x05\0\x03\x20", 10);
  const std::string key("\x04\x01\0\0\0\0\0\x61", 8);
  const std::string pointer("\x05\x01\0\x10\0\x20", 6);
  const std::string clipboard(
      "\x06\0\0\0\0\0\0\x03"
      "abc",
      11);
  CHECK(filter.Push(update + key + pointer + clipboard + update, output));
  CHECK(output == update + update);

  const std::string encodings("\x02\0\0\x01\0\0\0\0", 8);
  CHECK(filter.Push(encodings.substr(0, 3), output));
  CHECK(output.empty());
  CHECK(filter.Push(encodings.substr(3), output));
  CHECK(output == encodings);

  const std::string extended_clipboard(
      "\x06\0\0\0\xff\xff\xff\xfc"
      "data",
      12);
  CHECK(filter.Push(extended_clipboard, output));
  CHECK(output.empty());
  CHECK(!filter.Push(std::string("\x07", 1), output));
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
// Puts the fake X server, VNC and Chrome from tests/fixtures on `bin`.
void InstallFakeBrowser(const std::filesystem::path& bin) {
  namespace fs = std::filesystem;
  fs::create_directory(bin);
  for (const char* name : {"xauth", "Xtigervnc", "google-chrome-stable"}) {
    const auto path = bin / name;
    fs::copy_file(
        fs::path(UAGENT_TEST_SOURCE_DIR) / "tests/fixtures/browser_runtime.py",
        path);
    fs::permissions(path, fs::perms::owner_all);
  }
}
}  // namespace

void TestBrowserProfileSignIn() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-signin");
  const auto directory = fs::canonical(workspace.root) / "browser";
  const auto bin = workspace.root / "bin";
  InstallFakeBrowser(bin);
  ScopedEnv configured("UAGENT_BROWSER_DATA", directory.string());
  ScopedEnv search("PATH", bin.string() + ":" + getenv("PATH"));
  ScopedEnv display("DISPLAY");
  ScopedEnv authority("XAUTHORITY");
  constexpr const char* kDevice = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr const char* kOther = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  browser::Runtime runtime;
  CHECK(runtime.Execute({{"op", "setup_profile"}, {"device", kDevice}})
            .contains("error"));
  CHECK(runtime.Execute({{"op", "takeover"}, {"device", kDevice}})
            .value("running", false));
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
// control, and an agent call made meanwhile is reported as waiting.
void TestBrowserHandBackOnClose() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("browser-handback");
  const auto bin = workspace.root / "bin";
  InstallFakeBrowser(bin);
  ScopedEnv configured("UAGENT_BROWSER_DATA",
                       (fs::canonical(workspace.root) / "browser").string());
  ScopedEnv search("PATH", bin.string() + ":" + getenv("PATH"));
  ScopedEnv display("DISPLAY");
  ScopedEnv authority("XAUTHORITY");
  constexpr const char* kDevice = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr const char* kSession = "cccccccccccccccccccccccccccccccc";
  browser::Runtime runtime;
  auto taken = runtime.Execute({{"op", "takeover"}, {"device", kDevice}});
  CHECK(taken.value("mode", "") == "human");
  CHECK(!runtime.Execute({{"op", "viewer"}, {"role", "observe"}})
             .contains("error"));
  CHECK(runtime.Execute({{"op", "tabs"}, {"session_id", kSession}})
            .contains("error"));
  CHECK(runtime.Execute({{"op", "status"}}).value("waiting", false));
  auto closed = runtime.Execute({{"op", "viewer_disconnected"},
                                 {"device", kDevice},
                                 {"generation", taken["generation"]}});
  CHECK(closed.value("mode", "") == "idle");
  CHECK(!closed.value("waiting", true));
  CHECK(!runtime.Execute({{"op", "tabs"}, {"session_id", kSession}})
             .contains("error"));
  taken = runtime.Execute({{"op", "takeover"}, {"device", kDevice}});
  CHECK(taken.value("mode", "") == "human");
  closed = runtime.Execute({{"op", "viewer_disconnected"},
                            {"device", kDevice},
                            {"generation", taken["generation"]}});
  CHECK(closed.value("mode", "") == "agent");
  runtime.Shutdown();
}

}  // namespace uagent
