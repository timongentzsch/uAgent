// Copyright 2026 Timon Gentzsch

#include "include/core/settings_store.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "include/core/config.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

void Put(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

std::string Set(const std::string& folder, const std::string& name,
                const std::string& value) {
  return ChangeSettings(folder, [&](SettingValues& scope) {
    scope[name] = value;
    return std::string();
  });
}

}  // namespace

void TestSettingsStore() {
  TestWorkspace test("settings-store");
  const std::string folder = test.workspace.string();

  // Nothing saved: no document is made by reading.
  CHECK(ReadSettings(folder).all.empty());
  CHECK(!PathExists(SettingsPath()));

  // One scope changes at a time; the other folder and the rest stay. A name
  // that is no setting is what a value refers to as $NAME.
  CHECK(Set("", "MY_KEY", "secret").empty());
  CHECK(Set("", "UAGENT_BASE_URL", "http://one/v1/").empty());
  CHECK(Set("", "UAGENT_MODEL", "m2").empty());
  CHECK(Set(folder, "UAGENT_MAX_STEPS", "7").empty());
  CHECK(Set("/other", "UAGENT_MODEL", "m3").empty());
  SavedSettings saved = ReadSettings(folder);
  CHECK(saved.all.at("UAGENT_MODEL") == "m2" && saved.all.count("MY_KEY"));
  CHECK(saved.project == (SettingValues{{"UAGENT_MAX_STEPS", "7"}}));
  CHECK(ReadSettings("/other").project.at("UAGENT_MODEL") == "m3");

  // The document names a setting as people do and holds its value in its
  // own type; what a value refers to is kept apart under "variables".
  CHECK(Set("", "UAGENT_MAX_STEPS", "12").empty());
  CHECK(Set("", "UAGENT_SANDBOX", "relaxed").empty());
  CHECK(Set("", "UAGENT_API_KEY", "$MY_KEY").empty());
  const auto document = [] {
    return json::parse(ReadFile(SettingsPath(), 1 << 20).value_or(""));
  };
  CHECK(document()["format"] == 3);
  const json all = document()["all"];
  CHECK(all["model"] == "m2" && all["limits.maxSteps"] == 12);
  CHECK(all["sandbox.level"] == "relaxed");
  CHECK(all["endpoint.apiKey"] == "$MY_KEY");
  CHECK(all["variables"] == json({{"MY_KEY", "secret"}}));
  CHECK(!all.contains("UAGENT_MODEL") && !all.contains("MY_KEY"));
  CHECK(ReadSettings(folder).all.at("UAGENT_MAX_STEPS") == "12");
  CHECK(ReadSettings(folder).all.at("UAGENT_SANDBOX") == "relaxed");
  CHECK(ReadSettings(folder).warning.empty());
  for (const char* name :
       {"UAGENT_MAX_STEPS", "UAGENT_SANDBOX", "UAGENT_API_KEY"}) {
    CHECK(ChangeSettings("", [&](SettingValues& scope) {
            scope.erase(name);
            return std::string();
          }).empty());
  }
  // The schema an editor reads is written beside the document it names.
  CHECK(document()["$schema"] == "./settings.schema.json");
  const json schema = json::parse(
      ReadFile(test.home.string() + "/.uagent/config/settings.schema.json",
               1 << 20)
          .value_or(""));
  CHECK(
      schema["definitions"]["scope"]["properties"].contains("limits.maxSteps"));

  // A change that reports an error leaves everything as it was.
  CHECK(ChangeSettings("", [](SettingValues& scope) {
          scope.clear();
          return std::string("refused");
        }) == "refused");
  CHECK(ReadSettings(folder).all.size() == 3);

  // A document edited by hand: what is no setting, and a value its setting
  // does not take, are named and left out; the rest applies, and a later
  // save keeps what the author wrote.
  {
    TestWorkspace edited("settings-store-edited");
    Put(SettingsPath(),
        R"({"format": 3, "projects": {}, "all": {"model": "kept",)"
        R"( "limits.maxSteps": "many", "limits.maxToolCalls": -4,)"
        R"( "limits.maxTurnSeconds": false, "title.model": 7,)"
        R"( "sandbox.enabled": "off", "memory.enabled": 1,)"
        R"( "sandbox.enabld": true, "variables": {"A": 1}}})");
    SavedSettings read = ReadSettings("");
    CHECK(read.error.empty());
    // A value spelled as text is taken as written.
    CHECK(read.all ==
          (SettingValues{{"UAGENT_MODEL", "kept"}, {"UAGENT_SANDBOX", "off"}}));
    for (const char* named :
         {"limits.maxSteps expects an integer", "limits.maxToolCalls accepts",
          "limits.maxTurnSeconds expects a number", "title.model expects text",
          "memory.enabled expects true or false",
          "sandbox.enabld is not a setting", "variables.A must be text"}) {
      CHECK(read.warning.find(named) != std::string::npos);
    }
    CHECK(Set("", "UAGENT_MODEL", "changed").empty());
    const json kept = json::parse(ReadFile(SettingsPath(), 4096).value_or(""));
    CHECK(kept["all"]["model"] == "changed");
    CHECK(kept["all"]["sandbox.enabld"] == true);
    CHECK(kept["all"]["limits.maxSteps"] == "many");
    CHECK(kept["all"]["variables"]["A"] == 1);
    // What was not taken is not made takeable by a save of something else.
    CHECK(kept["all"]["memory.enabled"] == 1);
    CHECK(kept["all"]["sandbox.level"] == "off");
    // What the document holds is what a correction is compared with.
    CHECK(HeldSettings("").at("UAGENT_MAX_STEPS") == "many");
  }

  // The sandbox was a switch, saved as sandbox.enabled: its true and false
  // are the levels default and off, and a save writes the new name.
  {
    TestWorkspace switched("settings-store-sandbox-switch");
    Put(SettingsPath(), R"({"format": 3, "all": {"sandbox.enabled": false},)"
                        R"( "projects": {"/p": {"sandbox.enabled": true}}})");
    const SavedSettings read = ReadSettings("/p");
    CHECK(read.warning.empty());
    CHECK(read.all.at("UAGENT_SANDBOX") == "off");
    CHECK(read.project.at("UAGENT_SANDBOX") == "default");
    CHECK(Set("", "UAGENT_MODEL", "m").empty());
    const json rewritten =
        json::parse(ReadFile(SettingsPath(), 4096).value_or(""));
    CHECK(rewritten["all"]["sandbox.level"] == "off");
    CHECK(!rewritten["all"].contains("sandbox.enabled"));
  }

  // An editor takes no lock. What it saves while a change is being made is
  // kept, and the change is refused for another try.
  {
    TestWorkspace raced("settings-store-raced");
    CHECK(Set("", "UAGENT_MODEL", "first").empty());
    const std::string edited =
        R"({"format": 3, "projects": {}, "all": {"model": "by hand"}})";
    const std::string refused = ChangeSettings("", [&](SettingValues& scope) {
      Put(SettingsPath(), edited);
      scope["UAGENT_MAX_STEPS"] = "5";
      return std::string();
    });
    CHECK(refused.find("try again") != std::string::npos);
    CHECK(ReadFile(SettingsPath(), 4096).value_or("") == edited);
    CHECK(Set("", "UAGENT_MAX_STEPS", "5").empty());
    CHECK(ReadSettings("").all == (SettingValues{{"UAGENT_MAX_STEPS", "5"},
                                                 {"UAGENT_MODEL", "by hand"}}));
  }

  // A value that refers to another is judged by what that one holds: the
  // same problem as if it stood there itself. What it refers to may be
  // defined nowhere, and then nothing is set.
  {
    TestWorkspace referring("settings-store-referring");
    Put(SettingsPath(),
        R"({"format": 3, "projects": {}, "all": {"limits.maxSteps": "$COUNT",)"
        R"( "limits.maxToolCalls": "$CALLS", "limits.maxTurnSeconds": "$NONE",)"
        R"( "variables": {"COUNT": "many", "CALLS": "7"}}})");
    const SavedSettings read = ReadSettings("");
    CHECK(read.error.empty());
    CHECK(read.warning.find("limits.maxSteps expects an integer") !=
          std::string::npos);
    CHECK(read.warning.find("limits.maxToolCalls") == std::string::npos);
    CHECK(read.warning.find("limits.maxTurnSeconds") == std::string::npos);
    CHECK(!read.all.contains("UAGENT_MAX_STEPS"));
    CHECK(read.all.at("UAGENT_MAX_TOOL_CALLS") == "$CALLS");
    CHECK(HeldSettings("").at("UAGENT_MAX_STEPS") == "$COUNT");
  }

  // A document in the format before this one is rewritten in this one the
  // first time it is read, and one handed over in either is taken.
  {
    TestWorkspace earlier("settings-store-earlier");
    const json before = {
        {"format", 1},
        {"all",
         {{"UAGENT_MODEL", "old"}, {"UAGENT_MAX_STEPS", "9"}, {"TOKEN", "t"}}},
        {"projects",
         {{"/p",
           {{"UAGENT_SANDBOX", "false"},
            {"UAGENT_COORDINATOR_MAX_THREADS", "7"}}}}}};
    Put(SettingsPath(), JsonDump(before));
    CHECK(ReadSettings("/p").all == (SettingValues{{"TOKEN", "t"},
                                                   {"UAGENT_MAX_STEPS", "9"},
                                                   {"UAGENT_MODEL", "old"}}));
    CHECK(ReadSettings("/p").project.at("UAGENT_SANDBOX") == "false");
    const json now = json::parse(ReadFile(SettingsPath(), 4096).value_or(""));
    CHECK(now["format"] == 3 && now["all"]["limits.maxSteps"] == 9);
    CHECK(now["projects"]["/p"]["sandbox.level"] == "false");
    // A panel's settings under the names they had as a coordinator's.
    CHECK(now["projects"]["/p"]["panel.maxThreads"] == 7);
    AllSettings handed;
    std::string why;
    CHECK(ParseSettings(before, handed, why) && handed.all.size() == 3);
    CHECK(ParseSettings(now, handed, why) && handed.projects.count("/p"));
    // One handed over is taken whole or not at all.
    json wrong = now;
    wrong["all"]["no.such.setting"] = 1;
    CHECK(!ParseSettings(wrong, handed, why));
    CHECK(why.find("no.such.setting is not a setting") != std::string::npos);
    json spelled = now;
    spelled["all"]["coordinator.chatTurns"] = 2;
    Put(SettingsPath(), JsonDump(spelled));
    CHECK(ReadSettings("").all.at("UAGENT_PANEL_CHAT_TURNS") == "2");
  }

  // Format 2 kept OpenRouter's settings beside the others. They move into
  // that endpoint, a provider's protocol becomes its features, and the routing
  // variant joins the model id before its effort.
  {
    TestWorkspace endpoints("settings-store-endpoints");
    Put(SettingsPath(),
        R"({"format": 2, "all": {"openrouter.provider": "streamlake",)"
        R"( "openrouter.variant": "nitro", "model": "vendor/m:high",)"
        R"( "pdf.engine": "mistral-ocr", "webSearch.backend": "openrouter",)"
        R"( "providers": {"gateway": {"base_url": "https://gw.test/v1",)"
        R"( "protocol": "openrouter"}, "plain": {"base_url": "https://p.test",)"
        R"( "protocol": "anthropic"}}},)"
        R"( "projects": {"/p": {"openrouter.variant": "floor",)"
        R"( "openrouter.provider": "$WHO", "pdf.engine": "native"}}})");
    const SavedSettings read = ReadSettings("/p");
    CHECK(read.error.empty());
    const json now = json::parse(ReadFile(SettingsPath(), 8192).value_or(""));
    CHECK(now["format"] == 3);
    const json& held = now["all"];
    CHECK(held["model"] == "vendor/m:nitro:high");
    CHECK(held["webSearch.backend"] == "auto");
    CHECK(!held.contains("openrouter.provider") &&
          !held.contains("openrouter.variant") && !held.contains("pdf.engine"));
    const json& openrouter = held["providers"]["openrouter"];
    CHECK(openrouter["options"] ==
          (json{{"provider", "streamlake"}, {"pdf_engine", "mistral-ocr"}}));
    CHECK(!openrouter.contains("base_url"));
    const json& gateway = held["providers"]["gateway"];
    CHECK(!gateway.contains("protocol"));
    // What the global settings meant for it, an OpenRouter-protocol
    // provider, is its own options now.
    CHECK(gateway["options"] ==
          (json{{"pdf_engine", "mistral-ocr"}, {"provider", "streamlake"}}));
    CHECK(std::ranges::find(gateway["supports"], "prompt_cache_control") !=
          gateway["supports"].end());
    CHECK(!held["providers"]["plain"].contains("protocol"));
    CHECK(!held["providers"]["plain"].contains("supports"));
    // A project scope with no model drops the variant; a value that refers
    // to a variable stays, and is reported as the setting it no longer is.
    const json& project = now["projects"]["/p"];
    CHECK(!project.contains("openrouter.variant"));
    CHECK(project["openrouter.provider"] == "$WHO");
    CHECK(project["providers"]["openrouter"]["options"] ==
          (json{{"pdf_engine", "native"}}));
    CHECK(read.warning.find("openrouter.provider is not a setting") !=
          std::string::npos);
    // The environment still reads the migrated provider entry.
    CHECK(read.all.at("UAGENT_WEB_SEARCH_BACKEND") == "auto");
    AllSettings handed;
    std::string why;
    CHECK(ParseSettings(json::parse(R"({"format": 2, "projects": {},)"
                                    R"( "all": {"webSearch.backend":)"
                                    R"( "openrouter"}})"),
                        handed, why));
    CHECK(handed.all.at("UAGENT_WEB_SEARCH_BACKEND") == "auto");
  }

  // A variant belongs to the model an alias names; a model that is a
  // reference to a variable cannot take it, and the variant stays, reported.
  // The names the settings had in format 1 are read as the settings they were.
  {
    TestWorkspace aliased("settings-store-alias-variant");
    Put(SettingsPath(),
        R"({"format": 2, "all": {"model": "gw/fast:low",)"
        R"( "openrouter.variant": "floor", "providers": {"gw":)"
        R"( {"base_url": "https://gw.test", "protocol": "openrouter",)"
        R"( "models": {"fast": "vendor/m", "slow": {"id": "vendor/s"}}}}},)"
        R"( "projects": {"/p": {"model": "$CHOSEN",)"
        R"( "openrouter.variant": "nitro"}}})");
    const SavedSettings read = ReadSettings("/p");
    const json now = json::parse(ReadFile(SettingsPath(), 8192).value_or(""));
    CHECK(now["all"]["model"] == "gw/fast:low");
    CHECK(now["all"]["providers"]["gw"]["models"]["fast"] == "vendor/m:floor");
    CHECK(now["all"]["providers"]["gw"]["options"] ==
          (json{{"pdf_engine", "cloudflare-ai"}}));
    CHECK(now["projects"]["/p"]["model"] == "$CHOSEN");
    CHECK(now["projects"]["/p"]["openrouter.variant"] == "nitro");
    CHECK(read.warning.find("openrouter.variant is not a setting") !=
          std::string::npos);
  }
  {
    TestWorkspace names("settings-store-format-1-openrouter");
    Put(SettingsPath(), JsonDump(json{{"format", 1},
                                      {"all",
                                       {{"UAGENT_OPENROUTER_PROVIDER", "pin"},
                                        {"UAGENT_PDF_ENGINE", "native"},
                                        {"TOKEN", "t"}}},
                                      {"projects", json::object()}}));
    CHECK(ReadSettings("").error.empty());
    const json now = json::parse(ReadFile(SettingsPath(), 4096).value_or(""));
    CHECK(now["format"] == 3);
    CHECK(now["all"]["providers"]["openrouter"]["options"] ==
          (json{{"provider", "pin"}, {"pdf_engine", "native"}}));
    CHECK(now["all"]["variables"] == json({{"TOKEN", "t"}}));
  }

  // A document that cannot be read is reported and never saved over.
  for (const char* broken : {"{\"format\": 1, \"all\": [", "null"}) {
    Put(SettingsPath(), broken);
    CHECK(!ReadSettings(folder).error.empty());
    CHECK(!Set("", "UAGENT_MODEL", "m4").empty());
    CHECK(ReadFile(SettingsPath(), 1024).value_or("") == broken);
  }
}

}  // namespace uagent
