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
  namespace fs = std::filesystem;
  TestWorkspace test("settings-store");
  const std::string folder = test.workspace.string();
  const fs::path user = test.home / ".uagent/.config";
  const fs::path project = test.workspace / ".uagent/.config";
  const fs::path preference =
      test.home / ".uagent/config/model-preference.json";

  // Nothing saved, nothing to take over: no document is made by reading.
  CHECK(ReadSettings(folder).all.empty());
  CHECK(!PathExists(SettingsPath()));

  // The first read takes the user's file over, with the model an older
  // version remembered for the endpoint that file names, and a name that is
  // no setting (what a value refers to as $NAME) with it.
  Put(user,
      "UAGENT_BASE_URL=http://one/v1/\nMY_KEY=secret\n"
      "UAGENT_INTERNAL_DEPTH=3\n");
  Put(preference,
      R"({"format":1,"selection":"m1","base_url":"http://one/v1"})");
  Put(project, "UAGENT_MAX_STEPS=7\n");
  SavedSettings saved = ReadSettings(folder);
  CHECK(saved.all == (SettingValues{{"MY_KEY", "secret"},
                                    {"UAGENT_BASE_URL", "http://one/v1/"},
                                    {"UAGENT_MODEL", "m1"}}));
  CHECK(!PathExists(user.string()) && PathExists(user.string() + ".imported"));
  CHECK(!PathExists(preference.string()));
  // A project's file nobody approved is left where it is.
  CHECK(saved.project.empty() && PathExists(project.string()));

  // A file written again later is not read, and the backup is kept.
  Put(user, "UAGENT_MODEL=later\n");
  CHECK(ReadSettings(folder).all.at("UAGENT_MODEL") == "m1");
  CHECK(PathExists(user.string()));
  fs::remove(user);

  // Approved content is taken over; content changed since is not.
  std::string error;
  CHECK(WriteTrustRecord(folder,
                         {{"format", 3},
                          {"mcp", nullptr},
                          {"config", {{"UAGENT_MAX_STEPS", "9"}}}},
                         error));
  CHECK(ReadSettings(folder).project.empty());
  CHECK(WriteTrustRecord(folder,
                         {{"format", 3},
                          {"mcp", nullptr},
                          {"config", {{"UAGENT_MAX_STEPS", "7"}}}},
                         error));
  CHECK(ReadSettings(folder).project ==
        (SettingValues{{"UAGENT_MAX_STEPS", "7"}}));
  CHECK(PathExists(project.string() + ".imported"));

  // One scope changes at a time; the other folder and the rest stay.
  CHECK(Set("", "UAGENT_MODEL", "m2").empty());
  CHECK(Set("/other", "UAGENT_MODEL", "m3").empty());
  saved = ReadSettings(folder);
  CHECK(saved.all.at("UAGENT_MODEL") == "m2" && saved.all.count("MY_KEY"));
  CHECK(saved.project.count("UAGENT_MODEL") == 0);
  CHECK(ReadSettings("/other").project.at("UAGENT_MODEL") == "m3");

  // A change that reports an error leaves everything as it was.
  CHECK(ChangeSettings("", [](SettingValues& scope) {
          scope.clear();
          return std::string("refused");
        }) == "refused");
  CHECK(ReadSettings(folder).all.size() == 3);

  // A folder whose overrides were all reset is not imported a second time,
  // even where its file could not be archived.
  CHECK(ChangeSettings(folder, [](SettingValues& scope) {
          scope.clear();
          return std::string();
        }).empty());
  Put(project, "UAGENT_MAX_STEPS=7\n");
  CHECK(ReadSettings(folder, /*trusted=*/true).project.empty());

  // A file in a folder that cannot be written is taken over all the same;
  // its archiving stays owed and is done once the folder allows it, without
  // reading the file again.
  {
    const fs::path locked = test.root / "locked";
    Put(locked / ".uagent/.config", "UAGENT_MAX_STEPS=4\n");
    fs::permissions(locked / ".uagent",
                    fs::perms::owner_read | fs::perms::owner_exec);
    CHECK(ReadSettings(locked.string(), true).project.at("UAGENT_MAX_STEPS") ==
          "4");
    CHECK(PathExists((locked / ".uagent/.config").string()));
    CHECK(Set(locked.string(), "UAGENT_MAX_STEPS", "6").empty());
    fs::permissions(locked / ".uagent", fs::perms::owner_all);
    CHECK(ReadSettings(locked.string(), true).project.at("UAGENT_MAX_STEPS") ==
          "6");
    CHECK(!PathExists((locked / ".uagent/.config").string()));
    CHECK(PathExists((locked / ".uagent/.config.imported").string()));
    // Replacing everything saved while an archiving is owed does not make
    // the file new again, and a backup is never replaced.
    const fs::path owing = test.root / "owing";
    Put(owing / ".uagent/.config", "UAGENT_MAX_STEPS=8\n");
    Put(owing / ".uagent/.config.imported", "kept\n");
    fs::permissions(owing / ".uagent",
                    fs::perms::owner_read | fs::perms::owner_exec);
    CHECK(ReadSettings(owing.string(), true).project.size() == 1);
    std::string why;
    json everything = ExportSettings(why);
    everything["projects"].erase(owing.string());
    CHECK(ReplaceSettings(everything).empty());
    CHECK(ReadSettings(owing.string(), true).project.empty());
    fs::permissions(owing / ".uagent", fs::perms::owner_all);
    CHECK(ReadSettings(owing.string(), true).project.empty());
    CHECK(ReadFile((owing / ".uagent/.config.imported").string(), 64)
              .value_or("") == "kept\n");
    // What is owed is this host's own: it is no part of what is exported.
    std::string none;
    CHECK(!ExportSettings(none).contains("archive"));
  }

  // A folder the caller trusts needs no record.
  const fs::path flagged = test.root / "flagged";
  Put(flagged / ".uagent/.config", "UAGENT_MAX_STEPS=3\n");
  CHECK(ReadSettings(flagged.string()).project.empty());
  CHECK(ReadSettings(flagged.string(), /*trusted=*/true)
            .project.at("UAGENT_MAX_STEPS") == "3");

  // The remembered model is taken over without a config file too.
  {
    TestWorkspace fresh("settings-store-preference");
    Put(fresh.home / ".uagent/config/model-preference.json",
        R"({"format":1,"selection":"alone","route":true})");
    CHECK(ReadSettings("").all.at("UAGENT_MODEL") == "alone");
  }
  // A file that cannot be taken over is reported, stays, and nothing is
  // saved until it can be: a save would pass for the import done.
  {
    TestWorkspace big("settings-store-too-big");
    Put(big.home / ".uagent/.config",
        "BIG=" + std::string(5 * 1024 * 1024, 'x') + "\n");
    CHECK(!ReadSettings("").error.empty());
    CHECK(!Set("", "UAGENT_MODEL", "m").empty());
    CHECK(PathExists((big.home / ".uagent/.config").string()));
    CHECK(!PathExists(SettingsPath()));
  }

  // A document that cannot be read is reported and never saved over, with
  // or without a file waiting to be taken over.
  for (const char* broken : {"{\"format\": 1, \"all\": [", "null"}) {
    Put(SettingsPath(), broken);
    Put(user, "UAGENT_MODEL=waiting\n");
    fs::remove(user.string() + ".imported");
    CHECK(!ReadSettings(folder).error.empty());
    CHECK(!Set("", "UAGENT_MODEL", "m4").empty());
    CHECK(ReadFile(SettingsPath(), 1024).value_or("") == broken);
    CHECK(PathExists(user.string()));
  }
}

}  // namespace uagent
