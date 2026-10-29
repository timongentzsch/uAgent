// Copyright 2026 Timon Gentzsch

#include "include/core/sandbox.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/core/config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/sandbox_watch.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

bool HasRoot(const SandboxPolicy& policy, const std::string& root) {
  return std::find(policy.writable_roots.begin(), policy.writable_roots.end(),
                   root) != policy.writable_roots.end();
}

bool Rejected(const SandboxPolicyResult& result, const std::string& root) {
  return std::find(result.rejected.begin(), result.rejected.end(), root) !=
         result.rejected.end();
}

SandboxInputs BaseInputs() {
  SandboxInputs inputs;
  inputs.workspace = "/home/u/work";
  inputs.global_base = "/home/u/.uagent";
  inputs.tmpdir = "/private/var/folders/ab/T";
  inputs.tool_caches = {"/home/u/.cache", "/home/u/.local/share/uv"};
  inputs.terminal_logs = "/home/u/.uagent/terminals/logs";
  return inputs;
}

}  // namespace

void TestSandboxPolicy() {
  SandboxPolicyResult base = BuildSandboxPolicy(BaseInputs());
  CHECK(HasRoot(base.policy, "/home/u/work"));
  CHECK(HasRoot(base.policy, "/private/var/folders/ab/T"));
  CHECK(HasRoot(base.policy, "/home/u/.cache"));
  CHECK(HasRoot(base.policy, "/home/u/.local/share/uv"));
  // The detached log directory is the one deliberate hole inside ~/.uagent.
  CHECK(HasRoot(base.policy, "/home/u/.uagent/terminals/logs"));
  // /dev is granted on both platforms, not just Linux: under a blanket write
  // deny, `2>/dev/null` fails, and that is most shell commands.
  CHECK(HasRoot(base.policy, "/dev"));
  // What gives the project authority sits inside the writable workspace, so
  // it is carved back out by path rather than by leaving the workspace
  // ungranted.
  const std::vector<std::string> project_authority = {"/home/u/work/.mcp.json"};
  CHECK(base.policy.denied_writes == project_authority);
  // In a repository, its config and hooks are carved out too; before
  // `git init` they are not, so init still works.
  SandboxInputs repository = BaseInputs();
  repository.git_repository = true;
  CHECK(BuildSandboxPolicy(repository).policy.denied_writes ==
        (std::vector<std::string>{"/home/u/work/.mcp.json",
                                  "/home/u/work/.git/config",
                                  "/home/u/work/.git/hooks"}));
  CHECK(base.rejected.empty());
  CHECK(base.policy.allow_network);
  // A limit met as "Permission denied" is explained by naming the roots.
  CHECK(SandboxWritable(base.policy, "/home/u/work/out/a.png"));
  CHECK(!SandboxWritable(base.policy, "/home/u/workshop"));
  const std::string note = SandboxWritesNote(base.policy, "/home/u/work");
  CHECK(note.starts_with("commands may write only under "));
  CHECK(note.find("/home/u/work") != std::string::npos);
  CHECK(note.find("cannot") == std::string::npos);
  CHECK(note.find("network") == std::string::npos);
  SandboxPolicy offline = base.policy;
  offline.allow_network = false;
  CHECK(SandboxWritesNote(offline, "/home/u/work") ==
        note + ", and reach no network");
  // A home directory as the workspace holds ~/.uagent, so it is dropped: the
  // note then leads with that, and with what still works there.
  SandboxInputs at_home = BaseInputs();
  at_home.workspace = "/home/u";
  const SandboxPolicyResult home = BuildSandboxPolicy(at_home);
  CHECK(Rejected(home, "/home/u"));
  CHECK(!SandboxWritable(home.policy, "/home/u"));
  const std::string refused = SandboxWritesNote(home.policy, "/home/u");
  CHECK(refused.starts_with("commands cannot write the folder /home/u,"));
  CHECK(refused.find("/tmp") != std::string::npos);
  CHECK(refused.find("write_file") != std::string::npos);
  // A hidden path inside such a workspace is read around, folder by folder,
  // as a command starts: what is made there later, a running command cannot
  // read. Said only then, and never of a workspace that is writable.
  CHECK(refused.find("cannot read") == std::string::npos);
  SandboxPolicy hiding = home.policy;
  hiding.hidden = {"/home/u/.uagent/web"};
  CHECK(SandboxWritesNote(hiding, "/home/u").find("cannot read a file made") !=
        std::string::npos);
  SandboxPolicy project = base.policy;
  project.hidden = hiding.hidden;
  CHECK(SandboxWritesNote(project, "/home/u/work") == note);

  // The regression this whole design exists for: no root may be ~/.uagent or
  // an ancestor of it, or the config and the trust store come along with it.
  SandboxInputs reaching = BaseInputs();
  // Nor the folder inside it that holds the saved settings, or anything
  // under that.
  reaching.extra_roots = {"/home/u/.uagent",
                          "/home/u",
                          "/",
                          "/home/u/.uagent/..",
                          "/home/u/.uagent/config",
                          "/home/u/.uagent/config/x"};
  SandboxPolicyResult guarded = BuildSandboxPolicy(reaching);
  for (const std::string& root : guarded.policy.writable_roots) {
    CHECK(!SandboxPathWithin("/home/u/.uagent", root));
  }
  CHECK(Rejected(guarded, "/home/u/.uagent"));
  CHECK(Rejected(guarded, "/home/u"));
  CHECK(Rejected(guarded, "/"));
  CHECK(Rejected(guarded, "/home/u/.uagent/config"));
  CHECK(Rejected(guarded, "/home/u/.uagent/config/x"));

  // Relative roots are rejected rather than resolved: this code never touches
  // the filesystem, so it cannot know what one would resolve to.
  SandboxInputs relative = BaseInputs();
  relative.extra_roots = {"build", "./out", "/opt/ok"};
  SandboxPolicyResult filtered = BuildSandboxPolicy(relative);
  CHECK(Rejected(filtered, "build"));
  CHECK(Rejected(filtered, "./out"));
  CHECK(HasRoot(filtered.policy, "/opt/ok"));

  // A root nested in another is folded away, and duplicates with it, so the
  // profile is a function of the set rather than of discovery order.
  SandboxInputs nested = BaseInputs();
  nested.extra_roots = {"/home/u/work/sub", "/home/u/work", "/opt/a", "/opt/a"};
  SandboxPolicy folded = BuildSandboxPolicy(nested).policy;
  CHECK(!HasRoot(folded, "/home/u/work/sub"));
  CHECK(HasRoot(folded, "/home/u/work"));
  CHECK(std::count(folded.writable_roots.begin(), folded.writable_roots.end(),
                   "/opt/a") == 1);
  CHECK(std::is_sorted(folded.writable_roots.begin(),
                       folded.writable_roots.end()));

  // Prefix matching stops at a path separator in both directions.
  CHECK(SandboxPathWithin("/dev/null", "/dev"));
  CHECK(SandboxPathWithin("/dev", "/dev"));
  CHECK(!SandboxPathWithin("/devices", "/dev"));
  CHECK(!SandboxPathWithin("/de", "/dev"));
}

void TestSandboxLevels() {
  SandboxPreset preset = SandboxPreset::kOff;
  for (const auto& [text, expected] :
       {std::pair<const char*, SandboxPreset>{"readonly",
                                              SandboxPreset::kReadOnly},
        {"default", SandboxPreset::kDefault},
        {"relaxed", SandboxPreset::kRelaxed},
        {"off", SandboxPreset::kOff}}) {
    CHECK(ParseSandboxPreset(text, &preset));
    CHECK(preset == expected);
  }
  preset = SandboxPreset::kRelaxed;
  CHECK(!ParseSandboxPreset("loose", &preset));
  CHECK(!ParseSandboxPreset("0", &preset));
  CHECK(!ParseSandboxPreset("", &preset));
  CHECK(preset == SandboxPreset::kRelaxed);

  // Relaxed opens the home folder and withholds ~/.uagent from it; what is
  // granted inside ~/.uagent stays granted rather than folding into the home.
  SandboxInputs relaxed = BaseInputs();
  relaxed.open_home = "/home/u";
  const SandboxPolicyResult opened = BuildSandboxPolicy(relaxed);
  CHECK(HasRoot(opened.policy, "/home/u"));
  CHECK(!HasRoot(opened.policy, "/home/u/work"));
  CHECK(!HasRoot(opened.policy, "/home/u/.cache"));
  CHECK(HasRoot(opened.policy, "/home/u/.uagent/terminals/logs"));
  CHECK(opened.policy.withheld == std::vector<std::string>{"/home/u/.uagent"});
  CHECK(opened.rejected.empty());
  // A workspace kept in ~/.uagent is granted too.
  relaxed.workspace = "/home/u/.uagent/spaces/w";
  CHECK(
      HasRoot(BuildSandboxPolicy(relaxed).policy, "/home/u/.uagent/spaces/w"));
  // The extra roots keep their screen: ~/.uagent is still not one.
  relaxed.extra_roots = {"/home/u/.uagent"};
  CHECK(Rejected(BuildSandboxPolicy(relaxed), "/home/u/.uagent"));
  // Other levels withhold nothing.
  CHECK(BuildSandboxPolicy(BaseInputs()).policy.withheld.empty());

  // Readonly grants only temporary files and the terminal logs: not the
  // workspace, a package cache or a root the settings add.
  SandboxInputs looking = BaseInputs();
  looking.read_only = true;
  looking.extra_roots = {"/home/u/data"};
  // macOS's own temp folder is temporary too: its tools cache there.
  looking.user_tmpdir = "/private/var/folders/ab/xy/T";
  const SandboxPolicyResult only = BuildSandboxPolicy(looking);
  CHECK(only.policy.read_only);
  CHECK(!HasRoot(only.policy, "/home/u/work"));
  CHECK(!HasRoot(only.policy, "/home/u/.cache"));
  CHECK(!HasRoot(only.policy, "/home/u/data"));
  CHECK(HasRoot(only.policy, "/private/var/folders/ab/T"));
  CHECK(HasRoot(only.policy, "/tmp"));
  CHECK(HasRoot(only.policy, "/private/var/folders/ab/xy/T"));
  CHECK(HasRoot(only.policy, "/home/u/.uagent/terminals/logs"));
  CHECK(!SandboxWritable(only.policy, "/home/u/work/file"));
  CHECK(SandboxWritesNote(only.policy, "/home/u/work")
            .starts_with("commands here only read"));

  // Seatbelt: the denial follows the home allow, the nested roots are allowed
  // again after it, and the folder cannot be renamed away.
  const std::string profile = SeatbeltProfile(opened.policy);
  const size_t home_allow =
      profile.find("(allow file-write* (subpath \"/home/u\"))");
  const size_t withhold =
      profile.find("(deny file-write* (subpath \"/home/u/.uagent\"))");
  const size_t logs = profile.rfind(
      "(allow file-write* (subpath \"/home/u/.uagent/terminals/logs\"))");
  REQUIRE(home_allow != std::string::npos);
  REQUIRE(withhold != std::string::npos);
  REQUIRE(logs != std::string::npos);
  CHECK(home_allow < withhold && withhold < logs);
  CHECK(profile.find("(deny file-write-unlink (literal \"/home\") (literal "
                     "\"/home/u\"))") != std::string::npos);
  // The profile with every default hidden path still fits.
  SandboxPolicy crowded = opened.policy;
  crowded.hidden = {"/home/u/.uagent/config", "/home/u/.aws",
                    "/home/u/.config/gcloud", "/home/u/.azure",
                    "/home/u/.kube",          "/home/u/.gnupg",
                    "/home/u/.netrc",         "/home/u/.docker/config.json",
                    "/home/u/.uagent/web"};
  const std::string full = SeatbeltProfile(crowded);
  CHECK(!full.empty() && full.size() < kSeatbeltProfileLimit / 8);

  // The trampoline carries it.
  SandboxPolicy decoded;
  REQUIRE(SandboxPolicyFromJson(SandboxPolicyJson(crowded), &decoded));
  CHECK(decoded.hidden == crowded.hidden);
  CHECK(decoded.withheld == crowded.withheld);
}

// Yolo raises the default level to relaxed while it holds, and only that
// level; plan mode lowers it to readonly; switching back restores the default
// level's own status.
void TestSandboxFollowsApprovalMode() {
  CHECK(YoloSandboxPreset(SandboxPreset::kDefault) == SandboxPreset::kRelaxed);
  for (SandboxPreset kept : {SandboxPreset::kReadOnly, SandboxPreset::kRelaxed,
                             SandboxPreset::kOff}) {
    CHECK(YoloSandboxPreset(kept) == kept);
  }

  const ApprovalMode prior = CurrentApprovalMode();
  SetApprovalMode(ApprovalMode::kAsk);
  const SandboxStatus& asked = SandboxRuntime();
  if (asked.mode == SandboxMode::kEnforced && asked.preset == "default") {
    const std::string home = CanonicalAccessPath(UserHome()).string();
    const std::string base = CanonicalAccessPath(GlobalBase()).string();
    CHECK(!HasRoot(asked.policy, home));
    CHECK(asked.policy.withheld.empty());
    SetApprovalMode(ApprovalMode::kYolo);
    const SandboxStatus& raised = SandboxRuntime();
    CHECK(&raised != &asked);
    CHECK(raised.preset == "relaxed (yolo)");
    CHECK(raised.mode == SandboxMode::kEnforced);
    CHECK(raised.policy.allow_network == asked.policy.allow_network);
    CHECK(SandboxWritable(raised.policy, home + "/Documents/notes.txt"));
    CHECK(raised.policy.withheld == std::vector<std::string>{base});
    CHECK(raised.policy.unreadable.empty());
    CHECK(SandboxReadable(raised.policy, home + "/Documents/notes.txt"));
    // The file tools' work area follows it: the home folder, not ~/.uagent.
    CHECK(WithinWorkArea(home + "/Documents/notes.txt", "/nonexistent/ws"));
    CHECK(!WithinWorkArea(base + "/config/settings.json", "/nonexistent/ws"));
    // Composed once: asked again, the same status.
    CHECK(&SandboxRuntime() == &raised);
    CHECK(SandboxDiagnosticJson()["level"] == "relaxed (yolo)");
    // A child is started under it, whatever its own mode.
    const json inherited = json::parse(SandboxInheritance());
    CHECK(inherited["preset"] == "relaxed (yolo)");
    SandboxPolicy carried;
    REQUIRE(SandboxPolicyFromJson(inherited["policy"], &carried));
    CHECK(carried.withheld == raised.policy.withheld);
    SetApprovalMode(ApprovalMode::kPlan);
    const SandboxStatus& planning = SandboxRuntime();
    CHECK(planning.preset == "readonly (plan)");
    CHECK(planning.mode == SandboxMode::kEnforced);
    CHECK(planning.policy.read_only && !planning.policy.allow_network);
    CHECK(!SandboxWritable(planning.policy, CanonicalCwd() + "/notes.txt"));
    CHECK(planning.policy.unreadable == asked.policy.unreadable);
    CHECK(&SandboxRuntime() == &planning);
    SetApprovalMode(ApprovalMode::kAuto);
    CHECK(&SandboxRuntime() == &asked);
    CHECK(SandboxDiagnosticJson()["level"] == "default");
    CHECK(!WithinWorkArea(home + "/Documents/notes.txt", "/nonexistent/ws"));
  }
  SetApprovalMode(prior);
}

// Where Landlock cannot deny a write, it is undone and said.
void TestSandboxWatch() {
  namespace fs = std::filesystem;
  TestWorkspace test("watch");
  const fs::path hooks = test.workspace / ".git" / "hooks";
  const fs::path config = test.workspace / ".git" / "config";
  const fs::path mcp = test.workspace / ".mcp.json";
  fs::create_directories(hooks);
  WriteFile(hooks / "pre-commit.sample", "sample");
  WriteFile(config, "[core]\n");
  const std::string workspace = test.workspace.string();
  const SandboxWatch watch({mcp.string(), config.string(), hooks.string()});
  // Nothing changed, nothing said.
  CHECK(watch.Restore(workspace).empty());

  // A hook added, a config rewritten, a .mcp.json made that did not exist.
  WriteFile(hooks / "pre-commit", "#!/bin/sh\nevil\n");
  WriteFile(hooks / "pre-commit.sample", "changed");
  WriteFile(config, "[alias]\n");
  WriteFile(mcp, "{}");
  const std::vector<std::string> notices = watch.Restore(workspace);
  CHECK(notices.size() == 3);
  CHECK(
      std::find(notices.begin(), notices.end(),
                "restored .git/hooks: sandboxed commands may not change it") !=
      notices.end());
  CHECK(!fs::exists(mcp));
  CHECK(!fs::exists(hooks / "pre-commit"));
  CHECK(ReadFile((hooks / "pre-commit.sample").string(), 100).value_or("") ==
        "sample");
  CHECK(ReadFile(config.string(), 100).value_or("") == "[core]\n");
  CHECK(watch.Restore(workspace).empty());

  // A removed folder comes back.
  fs::remove_all(hooks);
  CHECK(watch.Restore(workspace).size() == 1);
  CHECK(ReadFile((hooks / "pre-commit.sample").string(), 100).value_or("") ==
        "sample");
  CHECK(SandboxRestoreNote(nullptr, workspace).empty());
}

void TestSandboxRendering() {
  SandboxPolicy policy;
  policy.writable_roots = {"/private/tmp", "/w"};
  policy.denied_writes = {"/w/.uagent"};
  policy.allow_network = true;
  std::string profile = SeatbeltProfile(policy);
  size_t deny_all = profile.find("(deny file-write*)\n");
  size_t allow_root = profile.find("(allow file-write* (subpath \"/w\"))");
  size_t override_deny =
      profile.find("(deny file-write* (subpath \"/w/.uagent\"))");
  REQUIRE(deny_all != std::string::npos);
  REQUIRE(allow_root != std::string::npos);
  REQUIRE(override_deny != std::string::npos);
  // Seatbelt takes the last matching rule, so this order is the policy: the
  // blanket deny must precede the allows, and a carve-out must follow them.
  CHECK(profile.find("(allow default)") < deny_all);
  CHECK(deny_all < allow_root);
  CHECK(allow_root < override_deny);
  CHECK(profile.find("(deny network*)") == std::string::npos);

  policy.allow_network = false;
  CHECK(SeatbeltProfile(policy).find("(deny network*)") != std::string::npos);

  // The browser profile is hidden after every allow, so a writable root that
  // contains it cannot grant it back, and its sockets refuse a connect.
  policy.writable_roots.emplace_back("/h/.local/share");
  policy.hidden = {"/h/.local/share/browser"};
  profile = SeatbeltProfile(policy);
  size_t hidden = profile.find(
      "(deny file-read* file-write* (subpath \"/h/.local/share/browser\"))");
  REQUIRE(hidden != std::string::npos);
  CHECK(profile.find("(allow file-write* (subpath \"/h/.local/share\"))") <
        hidden);
  CHECK(profile.find("(deny network-outbound (remote unix-socket (path-prefix "
                     "\"/h/.local/share/browser\")))") > hidden);
  // No ancestor can be renamed, which would carry it out from under the rule.
  CHECK(profile.find("(deny file-write-unlink (literal \"/h\") (literal "
                     "\"/h/.local\") (literal \"/h/.local/share\"))") !=
        std::string::npos);

  // Quotes and backslashes in a path must not end the SBPL string early.
  SandboxPolicy quoted;
  quoted.writable_roots = {"/w/we\"ird\\path"};
  CHECK(SeatbeltProfile(quoted).find("\"/w/we\\\"ird\\\\path\"") !=
        std::string::npos);

  // Over the ceiling the profile is refused, never truncated: a truncated
  // profile is a weaker one that would arrive without saying so.
  SandboxPolicy huge;
  for (int index = 0; index < 4000; ++index) {
    huge.writable_roots.push_back("/w/root-" + std::to_string(index));
  }
  CHECK(SeatbeltProfile(huge).empty());
  huge.writable_roots.resize(100);
  std::string bounded = SeatbeltProfile(huge);
  CHECK(!bounded.empty());
  CHECK(bounded.size() <= kSeatbeltProfileLimit);
}

// The person's own folders, the home folder's visible entries, close to
// reads, except the workspace and what commands use in them; dot-folders and
// everything outside the home folder stay open.
void TestSandboxReads() {
  using Paths = std::vector<std::string>;
  SandboxInputs inputs = BaseInputs();
  inputs.workspace = "/home/u/Software/work";
  inputs.closed_folders = {"/home/u/Software", "/home/u/Documents",
                           "/home/u/go", "/home/u/Library"};
  inputs.read_roots = {"/home/u/go", "/home/u/Library/Caches"};
  SandboxPolicy policy = BuildSandboxPolicy(inputs).policy;
  CHECK(policy.unreadable ==
        Paths({"/home/u/Documents", "/home/u/Library", "/home/u/Software"}));
  CHECK(policy.readable ==
        Paths({"/home/u/Library/Caches", "/home/u/Software/work"}));
  CHECK(SandboxReadable(policy, "/home/u/Software/work/src/a.cc"));
  CHECK(!SandboxReadable(policy, "/home/u/Software/other/a.cc"));
  CHECK(!SandboxReadable(policy, "/home/u/Documents/notes.md"));
  CHECK(SandboxReadable(policy, "/home/u/.gitconfig"));
  CHECK(SandboxReadable(policy, "/home/u/go/pkg/mod/x.go"));
  CHECK(SandboxReadable(policy, "/usr/include/stdio.h"));
  // Said with the folder a command reaches for first, and how to open one.
  const std::string note = SandboxWritesNote(policy, inputs.workspace);
  CHECK(note.find("such as /home/u/Software (sandbox.read") !=
        std::string::npos);

  // The readonly level writes no workspace, and still reads it.
  inputs.read_only = true;
  CHECK(SandboxReadable(BuildSandboxPolicy(inputs).policy,
                        "/home/u/Software/work/a.cc"));
  inputs.read_only = false;

  // Seatbelt takes the last matching rule: the folders close, the readable
  // ones open again inside them, and a hidden path closes after both.
  policy.hidden = {"/home/u/Software/work/.env"};
  const std::string profile = SeatbeltProfile(policy);
  const size_t closed =
      profile.find("(deny file-read-data (subpath \"/home/u/Software\"))");
  const size_t opened = profile.find(
      "(allow file-read-data (subpath \"/home/u/Software/work\"))");
  const size_t hidden = profile.find(
      "(deny file-read* file-write* (subpath \"/home/u/Software/work/.env\"))");
  REQUIRE(closed != std::string::npos);
  CHECK(closed < opened);
  CHECK(opened < hidden);
  CHECK(hidden != std::string::npos);

  // Both lists reach the Linux trampoline and a delegated child.
  SandboxPolicy decoded;
  REQUIRE(SandboxPolicyFromJson(SandboxPolicyJson(policy), &decoded));
  CHECK(decoded.unreadable == policy.unreadable);
  CHECK(decoded.readable == policy.readable);

  // In a container, projects sit side by side outside the home folder: the
  // workspace's neighbours close. One inside a closed folder adds nothing.
  SandboxInputs container = BaseInputs();
  container.workspace = "/workspaces/a";
  container.closed_folders = {"/workspaces/a", "/workspaces/b"};
  CHECK(BuildSandboxPolicy(container).policy.unreadable ==
        Paths({"/workspaces/b"}));
  inputs.closed_folders.emplace_back("/home/u/Software/other");
  CHECK(BuildSandboxPolicy(inputs).policy.unreadable == policy.unreadable);

  // A workspace that is the home folder opens every entry; so does a level
  // that collects none (relaxed, off).
  inputs.workspace = "/home/u";
  CHECK(BuildSandboxPolicy(inputs).policy.unreadable.empty());
  inputs.workspace = "/home/u/Software/work";
  inputs.closed_folders.clear();
  CHECK(BuildSandboxPolicy(inputs).policy.unreadable.empty());
}

void TestSandboxTrampolineArgs() {
  SandboxPolicy policy;
  policy.writable_roots = {"/w", "/private/tmp"};
  policy.denied_writes = {"/w/.mcp.json"};
  policy.hidden = {"/b/browser"};
  policy.allow_network = false;
  policy.read_only = true;
  SandboxPolicy decoded;
  REQUIRE(SandboxPolicyFromJson(SandboxPolicyJson(policy), &decoded));
  CHECK(decoded.writable_roots == policy.writable_roots);
  CHECK(decoded.denied_writes == policy.denied_writes);
  CHECK(decoded.hidden == policy.hidden);
  CHECK(!decoded.allow_network);
  CHECK(decoded.read_only);

  SandboxPolicy allowed;
  allowed.allow_network = true;
  SandboxPolicy round;
  REQUIRE(SandboxPolicyFromJson(SandboxPolicyJson(allowed), &round));
  CHECK(round.allow_network);
  CHECK(round.writable_roots.empty());

  // Every malformed form has to fail rather than decode to an empty policy,
  // which the trampoline would enforce as no confinement at all.
  SandboxPolicy ignored;
  CHECK(!SandboxPolicyFromJson(json(), &ignored));
  CHECK(!SandboxPolicyFromJson(json::object(), &ignored));
  CHECK(!SandboxPolicyFromJson({{"network", "yes"}, {"roots", json::array()}},
                               &ignored));
  CHECK(!SandboxPolicyFromJson({{"network", true}}, &ignored));
  CHECK(!SandboxPolicyFromJson({{"network", true}, {"roots", "/w"}}, &ignored));
  CHECK(!SandboxPolicyFromJson({{"network", true}, {"roots", {"/w", 1}}},
                               &ignored));
  CHECK(!SandboxPolicyFromJson(
      {{"network", true}, {"roots", json::array()}, {"unreadable", "/h"}},
      &ignored));

  // The wrapper has to name a program the spawn can actually exec. It once
  // took argv[0], which only Bootstrap records, so every binary that links
  // this library without running it -- this test binary included -- produced a
  // wrapper whose argv[0] was "" and failed every sandboxed spawn with ENOENT.
  const SandboxStatus& status = SandboxRuntime();
  std::vector<std::string> wrapper = SandboxWrapperArgv(status);
  if (status.mode == SandboxMode::kEnforced) {
    REQUIRE(!wrapper.empty());
    CHECK(!wrapper.front().empty());
    CHECK(wrapper.front().front() == '/');
    CHECK(access(wrapper.front().c_str(), X_OK) == 0);
#if defined(__linux__)
    // The policy travels as one argv word, so one past the kernel's limit on a
    // word is refused rather than truncated.
    SandboxStatus crowded = status;
    for (int index = 0; index < 5000; ++index) {
      crowded.policy.unreadable.push_back(
          "/home/u/folder-" + std::to_string(index) + std::string(20, 'x'));
    }
    CHECK(SandboxWrapperArgv(crowded).empty());
#endif
  } else {
    CHECK(wrapper.empty());
  }
}

// What is hidden, canonical, only while it exists.
void TestSandboxHiddenPaths() {
  namespace fs = std::filesystem;
  TestWorkspace test("hidden");
  const fs::path home = fs::canonical(test.home);
  const fs::path browser = home / "browser";
  const auto has = [](const std::vector<std::string>& paths,
                      const fs::path& path) {
    return std::find(paths.begin(), paths.end(), path.string()) != paths.end();
  };
  {
    ScopedEnv unset("UAGENT_BROWSER_DATA");
    CHECK(HiddenPaths().empty());
  }
  ScopedEnv configured("UAGENT_BROWSER_DATA", browser.string());
  CHECK(HiddenPaths().empty());
  fs::create_directories(browser);
  CHECK(HiddenPaths() == std::vector<std::string>{browser.string()});

  // The credential stores that exist, and not ~/.ssh or the GitHub CLI login,
  // which are agent work.
  for (const char* store : {".aws", ".kube", ".ssh", ".config/gh",
                            ".config/mycloud", ".uagent/config"}) {
    fs::create_directories(home / store);
  }
  WriteFile(home / ".netrc", "machine x");
  fs::create_directories(home / ".docker");
  WriteFile(home / ".docker" / "config.json", "{}");
  std::vector<std::string> hidden = HiddenPaths();
  for (const char* store :
       {".aws", ".kube", ".netrc", ".docker/config.json", ".uagent/config"}) {
    CHECK(has(hidden, home / store));
  }
  CHECK(!has(hidden, home / ".ssh"));
  CHECK(!has(hidden, home / ".config" / "gh"));
  CHECK(!has(hidden, home / ".azure"));  // not there, so not listed
  CHECK(!has(hidden, home / ".config" / "mycloud"));

  // sandbox.hide adds, with ~/ for the home folder; a relative entry names
  // nothing. sandbox.reveal takes a default away.
  ScopedEnv hide("UAGENT_SANDBOX_HIDE", "~/.config/mycloud:rel/path");
  ScopedEnv reveal("UAGENT_SANDBOX_REVEAL", (home / ".aws").string());
  hidden = HiddenPaths();
  CHECK(has(hidden, home / ".config" / "mycloud"));
  CHECK(!has(hidden, home / ".aws"));
  CHECK(has(hidden, home / ".kube"));
  CHECK(hidden.size() == 6);
}

void TestSandboxProbe() {
  // The probe reads the host, so what it reports is not assertable here -- what
  // is, is that it answers at all and answers the same way twice. A probe that
  // re-read the host per spawn would be both slower and free to change its mind
  // mid-session, and every caller treats the level as fixed.
  const SandboxLevel level = SandboxSupported();
  CHECK(level == SandboxSupported());
  CHECK(level == SandboxLevel::kUnavailable ||
        level == SandboxLevel::kFilesystem ||
        level == SandboxLevel::kFilesystemAndNetwork);
#if defined(__APPLE__)
  // sandbox-exec is part of the base system, so the only macOS host that can
  // report unavailable is one where it has been removed.
  CHECK(level == SandboxLevel::kFilesystemAndNetwork);
#endif
}

}  // namespace uagent
