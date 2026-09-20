// Copyright 2026 Timon Gentzsch

#include <filesystem>
#include <fstream>
#include <string>

#include "include/browser/browser.h"
#include "include/browser/runtime.h"
#include "include/core/json.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestBrowserHandoverRecovery() {
  TestWorkspace workspace("browser-handover");
  auto directory = std::filesystem::canonical(workspace.root) / "browser";
  ScopedEnv configured("UAGENT_BROWSER_DATA", directory.string());
  CHECK(browser::EnsureDataDirectory(directory.string()));
  constexpr const char* kSession = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr const char* kInteraction = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  {
    std::ofstream saved(directory / "handover.json");
    saved << JsonDump({{"session_id", kSession},
                       {"interaction_id", kInteraction}});
  }
  browser::Runtime runtime;
  json status = runtime.Execute({{"op", "status"}});
  CHECK(status.value("mode", "") == "human");
  CHECK(!status.value("running", true));
  CHECK(runtime.Execute({{"op", "agent_status"}, {"session_id", kSession}})
            .contains("error"));
  CHECK(runtime.Execute({{"op", "cancel_handover"},
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

}  // namespace uagent
