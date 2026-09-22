// Copyright 2026 Timon Gentzsch

#include "include/browser/browser.h"

#include <filesystem>
#include <fstream>
#include <string>

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

}  // namespace uagent
