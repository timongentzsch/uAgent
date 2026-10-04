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
  Put(preference, R"({"selection":"m1","base_url":"http://one/v1"})");
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

  // A folder the caller trusts needs no record.
  const fs::path flagged = test.root / "flagged";
  Put(flagged / ".uagent/.config", "UAGENT_MAX_STEPS=3\n");
  CHECK(ReadSettings(flagged.string()).project.empty());
  CHECK(ReadSettings(flagged.string(), /*trusted=*/true)
            .project.at("UAGENT_MAX_STEPS") == "3");

  // A document that cannot be read is reported and never saved over.
  Put(SettingsPath(), "{\"format\": 1, \"all\": [");
  CHECK(!ReadSettings(folder).error.empty());
  CHECK(!Set("", "UAGENT_MODEL", "m4").empty());
  CHECK(ReadFile(SettingsPath(), 1024).value_or("") ==
        "{\"format\": 1, \"all\": [");
}

}  // namespace uagent
