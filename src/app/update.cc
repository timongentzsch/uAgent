// Copyright 2026 Timon Gentzsch

#include "include/app/update.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "include/api/client.h"
#include "include/core/capture.h"
#include "include/core/child_env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/private_store.h"
#include "include/core/runtime_config.h"
#include "include/core/signals.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
namespace fs = std::filesystem;
namespace {

constexpr char kRepository[] = "timongentzsch/uAgent";
constexpr char kCacheFile[] = "update.json";
constexpr size_t kReleaseBytes = KiB(256);
constexpr size_t kNotesBytes = KiB(16);
// An install built from source rebuilds µAgent, which takes minutes.
constexpr int kInstallSeconds = 1800;

// A test's stand-in for GitHub: a folder with latest.json and install.sh.
std::string Fixture() {
  const char* folder = getenv("UAGENT_INTERNAL_RELEASES");
  return folder ? folder : "";
}

std::string CachePath() { return UagentDir(kConfigDir) + "/" + kCacheFile; }

// "1.10.0" as its numbers; none for a build that is no release's.
std::vector<int64_t> Numbers(std::string_view version) {
  std::vector<int64_t> numbers(1, 0);
  bool digit = false;
  for (const char c : version) {
    if (c == '.' && digit) {
      numbers.push_back(0);
      digit = false;
    } else if (c >= '0' && c <= '9' && numbers.back() < 100'000'000) {
      numbers.back() = numbers.back() * 10 + (c - '0');
      digit = true;
    } else {
      return {};
    }
  }
  return digit ? numbers : std::vector<int64_t>{};
}

bool Newer(std::string_view latest, std::string_view current) {
  const auto have = Numbers(current);
  return !have.empty() && Numbers(latest) > have;
}

// The folder holding bin/uagent and share/uagent, when this binary sits in
// one its user may write; else empty.
std::string Prefix() {
  std::error_code ec;
  const fs::path bin = fs::canonical(ExecutablePath(), ec).parent_path();
  if (ec || bin.filename() != "bin") return "";
  const fs::path prefix = bin.parent_path();
  return access(bin.c_str(), W_OK) == 0 &&
                 access((prefix / "share").c_str(), W_OK) == 0
             ? prefix.string()
             : "";
}

std::string Install() {
  return Trim(ReadFile(InstalledShare("install").string(), 64).value_or(""));
}

std::string Command(const std::string& install) {
  if (install == "checkout") return "git pull && ./install.sh";
  return std::string("curl -fsSL https://raw.githubusercontent.com/") +
         kRepository + "/master/install.sh | " +
         (install == "container" ? "UAGENT_INSTALL=container " : "") + "sh";
}

}  // namespace

json UpdateState(bool terminal) {
  json state = ReadJsonFile(CachePath(), kReleaseBytes);
  if (!state.is_object()) state = json::object();
  const std::string install = Install();
  const bool available = Newer(JsonValue(state, "latest", ""), kVersion);
  // Only an installer's own install is replaced by an installer.
  const bool here = install == "release" || install == "source";
  const std::string blocked = !here              ? ""
                              : !terminal        ? "terminal"
                              : Prefix().empty() ? "unwritable"
                                                 : "";
  state["version"] = kVersion;
  state["install"] = install;
  state["available"] = available;
  state["command"] = Command(install);
  state["blocked"] = blocked;
  state["apply"] = here && available && blocked.empty();
  return state;
}

json CheckForUpdate(bool terminal) {
  std::string text, error;
  if (const std::string fixture = Fixture(); !fixture.empty()) {
    text = ReadFile(fixture + "/latest.json", kReleaseBytes).value_or("");
  } else {
    Api api(RuntimeConfig::FromEnvironment());
    WebResponse answer =
        api.GetUrl(std::string("https://api.github.com/repos/") + kRepository +
                       "/releases/latest",
                   15, kReleaseBytes);
    text = std::move(answer.body);
    error = std::move(answer.error);
  }
  const json release = json::parse(text, nullptr, false);
  const std::string tag = JsonValue(release, "tag_name", "");
  const std::string version = tag.starts_with("v") ? tag.substr(1) : tag;
  if (error.empty() && Numbers(version).empty()) {
    error = "GitHub named no release";
  }
  if (error.empty()) {
    const std::string url = JsonValue(release, "html_url", "");
    WritePrivateFile(
        CachePath(),
        JsonDump(
            {{"latest", version},
             {"notes", Utf8Trunc(JsonValue(release, "body", ""), kNotesBytes)},
             {"url", url.starts_with("https://github.com/") ? url : ""},
             {"published", JsonValue(release, "published_at", "")},
             {"checked_ms", NowMillis()}}),
        error);
  }
  json state = UpdateState(terminal);
  if (!error.empty()) state["error"] = error;
  return state;
}

std::string ApplyUpdate(const std::string& version, bool terminal,
                        std::string& error) {
  const json state = UpdateState(terminal);
  const std::string prefix = Prefix();
  // Only the release GitHub named at the last check, and only over an
  // install an installer made.
  if (!JsonValue(state, "apply", false) ||
      version != JsonValue(state, "latest", "")) {
    error = "this install cannot be updated from here";
    return "";
  }
  std::string script;
  if (const std::string fixture = Fixture(); !fixture.empty()) {
    script = ReadFile(fixture + "/install.sh", kReleaseBytes).value_or("");
  } else {
    Api api(RuntimeConfig::FromEnvironment());
    WebResponse answer =
        api.GetUrl(std::string("https://raw.githubusercontent.com/") +
                       kRepository + "/v" + version + "/install.sh",
                   30, kReleaseBytes);
    if (answer.error.empty() && !answer.truncated) {
      script = std::move(answer.body);
    }
  }
  if (!script.starts_with("#!/bin/sh")) {
    error = "the installer of " + version + " could not be read";
    return "";
  }
  // Asking nothing, the installer replaces the files and leaves the service
  // alone: the host restarts itself.
  ChildEnvironment environment({{"UAGENT_NONINTERACTIVE", "1"},
                                {"UAGENT_INSTALL", "here"},
                                {"UAGENT_VERSION", version},
                                {"UAGENT_PREFIX", prefix}});
  const CapturedProcess ran = CaptureProcess({"/bin/sh", "-s"}, kInstallSeconds,
                                             script, environment.Data());
  if (!ran.Ok()) {
    error = ran.error.empty() ? "the installer failed" : ran.error;
  }
  return ran.output;
}

}  // namespace uagent
