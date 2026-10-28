// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include "include/core/config_registry.h"
#include "include/core/defaults.h"
#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/sandbox.h"
#include "include/core/settings_store.h"
#include "include/core/signals.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/core/thread_annotations.h"

// Everything that has to ask the host what it can enforce, and the Linux
// trampoline that enforces it. The platform split lives inside this file so
// that sandbox.h stays ifdef-free and sandbox.cc stays pure.

namespace uagent {
namespace {

#if defined(__linux__)
// Re-exec the running inode, including after an upgrade unlinks its original
// path. Resolving the link would instead return a nonexistent "(deleted)"
// filename, or start a newer image with a different sandbox protocol.
std::string SelfExecutablePath() {
  return access("/proc/self/exe", X_OK) == 0 ? "/proc/self/exe"
                                             : ExecutablePath();
}
#endif

#if defined(__linux__)

// Landlock has no libc wrappers, and <linux/landlock.h> is missing on distros
// whose headers predate a kernel that supports it -- so the numbers live here
// instead, and the probe below decides which of them this kernel understands.
// The struct is declared with every field any ABI we handle knows about; the
// size passed to the kernel is what selects the ABI, which is why the create
// call sends the prefix an older kernel expects rather than the whole thing.
constexpr int kCreateRuleset = 444;
constexpr int kAddRule = 445;
constexpr int kRestrictSelf = 446;
constexpr uint32_t kRulesetVersion = 1U << 0;
constexpr uint32_t kRulePathBeneath = 1;

// Read, directory-read and execute are deliberately absent: an access right
// left out of handled_access_fs is never checked, which leaves reads exactly as
// unrestricted as the macOS profile's (allow default) leaves them -- except
// under a hidden or unreadable path, below. It also keeps every rule below
// grantable, since allowed_access must be a subset of what the ruleset
// handles.
constexpr uint64_t kWriteAccess = (1ULL << 1) |   // WRITE_FILE
                                  (1ULL << 4) |   // REMOVE_DIR
                                  (1ULL << 5) |   // REMOVE_FILE
                                  (1ULL << 6) |   // MAKE_CHAR
                                  (1ULL << 7) |   // MAKE_DIR
                                  (1ULL << 8) |   // MAKE_REG
                                  (1ULL << 9) |   // MAKE_SOCK
                                  (1ULL << 10) |  // MAKE_FIFO
                                  (1ULL << 11) |  // MAKE_BLOCK
                                  (1ULL << 12);   // MAKE_SYM
// Renaming and hard-linking across two directories. Handling it costs nothing
// and granting it restores what ABI 1 forbids outright, so a `mv` between two
// writable roots keeps working.
constexpr uint64_t kAccessRefer = 1ULL << 13;
constexpr uint64_t kAccessTruncate = 1ULL << 14;
// IOCTL_DEV (bit 15) is never handled: the tty ioctls an interactive command
// issues are not writes to confine, and handling the bit would break them.
// Reading a file, and from ABI 9 connecting to a unix socket, are handled
// only to hide or close paths, and granted everywhere else. Listing stays
// unhandled: a name is not a secret, and handling it would make every ancestor
// of a hidden path unlistable.
constexpr uint64_t kAccessReadFile = 1ULL << 2;
constexpr uint64_t kAccessResolveUnix = 1ULL << 16;
// The rights a rule on a non-directory may carry: EXECUTE, WRITE_FILE,
// READ_FILE, TRUNCATE, IOCTL_DEV and RESOLVE_UNIX.
constexpr uint64_t kFileAccess = (1ULL << 0) | (1ULL << 1) | kAccessReadFile |
                                 kAccessTruncate | (1ULL << 15) |
                                 kAccessResolveUnix;

constexpr uint64_t kAccessBindTcp = 1ULL << 0;
constexpr uint64_t kAccessConnectTcp = 1ULL << 1;

struct RulesetAttr {
  uint64_t handled_access_fs;
  uint64_t handled_access_net;
};

struct PathBeneathAttr {
  uint64_t allowed_access;
  int32_t parent_fd;
} __attribute__((packed));

// Negative when this kernel has no Landlock at all.
int LandlockAbi() {
  static const int kAbi = static_cast<int>(
      syscall(kCreateRuleset, nullptr, size_t{0}, kRulesetVersion));
  return kAbi;
}

uint64_t HandledFsAccess(int abi) {
  uint64_t handled = kWriteAccess;
  if (abi >= 2) handled |= kAccessRefer;
  if (abi >= 3) handled |= kAccessTruncate;
  return handled;
}

// Reports the reason and leaves the caller to exit without executing, which is
// the only safe response: a command that reaches execvp() past a failure here
// is a command running with no confinement at all.
bool Fatal(const char* what) {
  fprintf(stderr, "uagent: sandbox: %s failed (errno %d)\n", what, errno);
  return false;
}

// Grants `access` beneath `root`, except beneath any `skipped` path. Landlock
// has no deny form, so a root holding a skipped path is granted child by
// child, recursing only down the one branch that leads to it. Rules bind to
// inodes, so renaming a skipped directory later does not uncover it.
bool AddRulesExcept(int ruleset, const std::string& root, uint64_t access,
                    const std::vector<std::string>& skipped) {
  bool holds_hidden = false;
  for (const std::string& path : skipped) {
    if (SandboxPathWithin(root, path)) return true;
    holds_hidden = holds_hidden || SandboxPathWithin(path, root);
  }
  // Not followed: a symlink's target is granted, or not, where it really is.
  Fd parent(open(root.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  // A root that does not exist grants nothing, which is stricter than the
  // policy asked for and so cannot open a hole. Skipping it keeps optional
  // roots -- an unset $TMPDIR, a cache dir nobody has created yet -- from
  // turning into a refusal to run.
  if (!parent || fstat(parent.Get(), &info) != 0 || S_ISLNK(info.st_mode)) {
    return true;
  }
  if (!holds_hidden) {
    PathBeneathAttr rule{};
    rule.allowed_access = S_ISDIR(info.st_mode) ? access : access & kFileAccess;
    rule.parent_fd = parent.Get();
    if (rule.allowed_access != 0 &&
        syscall(kAddRule, ruleset, kRulePathBeneath, &rule, 0U) != 0) {
      return Fatal("landlock_add_rule");
    }
    return true;
  }
  DIR* directory = opendir(root.c_str());
  if (!directory) return Fatal("opendir");
  std::vector<std::string> children;
  while (const dirent* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    children.push_back(root == "/" ? "/" + name : root + "/" + name);
  }
  closedir(directory);
  for (const std::string& child : children) {
    if (!AddRulesExcept(ruleset, child, access, skipped)) return false;
  }
  return true;
}

bool ApplyLandlock(const SandboxPolicy& policy) {
  const int abi = LandlockAbi();
  if (abi < 0) return Fatal("landlock probe");
  if (!policy.allow_network && abi < 4) return Fatal("network confinement");

  // Reading, and from ABI 9 connecting to a unix socket, are handled only to
  // close paths: granted everywhere except beneath a hidden or unreadable
  // one, and again inside the readable folders of an unreadable one.
  const uint64_t reads =
      policy.hidden.empty() && policy.unreadable.empty()
          ? 0
          : kAccessReadFile | (abi >= 9 ? kAccessResolveUnix : 0);
  RulesetAttr attr{};
  attr.handled_access_fs = HandledFsAccess(abi) | reads;
  if (!policy.allow_network) {
    attr.handled_access_net = kAccessBindTcp | kAccessConnectTcp;
  }
  // An older kernel rejects a size it does not recognise, so send the prefix
  // it knows: the net field only exists from ABI 4 on.
  const size_t attr_size =
      abi >= 4 ? sizeof(attr) : sizeof(attr.handled_access_fs);
  Fd ruleset(static_cast<int>(syscall(kCreateRuleset, &attr, attr_size, 0U)));
  if (!ruleset) return Fatal("landlock_create_ruleset");

  // A writable root that holds a hidden or withheld path is granted around it
  // too. A withheld path that holds the root is no reason to skip it: that is
  // a root granted inside one, such as the terminal logs.
  for (const std::string& root : policy.writable_roots) {
    std::vector<std::string> skipped = policy.hidden;
    for (const std::string& path : policy.withheld) {
      if (!SandboxPathWithin(root, path)) skipped.push_back(path);
    }
    if (!AddRulesExcept(ruleset.Get(), root, HandledFsAccess(abi), skipped)) {
      return false;
    }
  }
  if (reads) {
    std::vector<std::string> closed = policy.hidden;
    closed.insert(closed.end(), policy.unreadable.begin(),
                  policy.unreadable.end());
    if (!AddRulesExcept(ruleset.Get(), "/", reads, closed)) return false;
    for (const std::string& open : policy.readable) {
      if (!AddRulesExcept(ruleset.Get(), open, reads, policy.hidden)) {
        return false;
      }
    }
  }
  // No net rule is ever added: handling the two TCP access rights with no rule
  // granting them is itself the deny, and an allow-network policy handles
  // nothing, so there is no port left to permit.

  // Must precede restrict_self, and blocks the setuid escape a confined
  // process would otherwise keep. It is also why `ping` stops working.
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return Fatal("PR_SET_NO_NEW_PRIVS");
  }
  if (syscall(kRestrictSelf, ruleset.Get(), 0U) != 0) {
    return Fatal("landlock_restrict_self");
  }
  return true;
}

#endif  // __linux__

}  // namespace

SandboxLevel SandboxSupported() {
#if defined(__APPLE__)
  static const SandboxLevel kLevel = access("/usr/bin/sandbox-exec", X_OK) == 0
                                         ? SandboxLevel::kFilesystemAndNetwork
                                         : SandboxLevel::kUnavailable;
  return kLevel;
#elif defined(__linux__)
  const int abi = LandlockAbi();
  if (abi < 0) return SandboxLevel::kUnavailable;
  return abi >= 4 ? SandboxLevel::kFilesystemAndNetwork
                  : SandboxLevel::kFilesystem;
#else
  return SandboxLevel::kUnavailable;
#endif
}

namespace {

// A configured path may start with ~/. Relative paths otherwise are the
// caller's to refuse or resolve.
std::string ExpandHome(std::string path) {
  const std::string home = UserHome();
  if (path.starts_with("~/") && !home.empty()) path = home + path.substr(1);
  return path;
}

std::string TrimLine(std::string_view text) {
  return Trim(std::string(text.substr(0, text.find_first_of("\r\n"))));
}

struct GitWorktree {
  std::string git_dir;     // <main>/.git/worktrees/<name>
  std::string common_dir;  // <main>/.git
};

// A linked worktree keeps its git directory, and the repository's shared one,
// outside the folder: its `.git` is a file naming them. Only a genuine pair is
// believed -- the directory must be a child of the shared one's `worktrees`
// and point back at this `.git` -- because a hostile repository could
// otherwise name any folder and have it opened. Empty when `folder` and the
// folders above it hold no such file.
GitWorktree FindGitWorktree(const std::string& folder) {
  namespace fs = std::filesystem;
  std::error_code error;
  for (fs::path dir = folder; dir.has_relative_path() && dir != dir.root_path();
       dir = dir.parent_path()) {
    const fs::path dot_git = dir / ".git";
    if (!fs::exists(dot_git, error)) continue;
    if (!fs::is_regular_file(dot_git, error)) return {};
    const std::string line =
        TrimLine(ReadFile(dot_git.string(), 4096).value_or(""));
    constexpr std::string_view kPrefix = "gitdir:";
    if (!line.starts_with(kPrefix)) return {};
    fs::path git_dir = Trim(line.substr(kPrefix.size()));
    if (git_dir.is_relative()) git_dir = dir / git_dir;
    git_dir = CanonicalAccessPath(git_dir.string());
    const fs::path worktrees = git_dir.parent_path();
    const fs::path common_dir = worktrees.parent_path();
    auto line_of = [&](const char* name) {
      return TrimLine(ReadFile((git_dir / name).string(), 4096).value_or(""));
    };
    fs::path shared = line_of("commondir");
    if (shared.is_relative()) shared = git_dir / shared;
    if (worktrees.filename() != "worktrees" ||
        !fs::is_directory(common_dir / "objects", error) ||
        CanonicalAccessPath(shared.string()) != common_dir ||
        CanonicalAccessPath(line_of("gitdir")) !=
            CanonicalAccessPath(dot_git.string())) {
      return {};
    }
    return {git_dir.string(), common_dir.string()};
  }
  return {};
}

// Closed: the visible entries of the home folder, of ~/.uagent/spaces and of
// the folder that holds the folder, whose other entries are other
// projects. Open inside them: the skill folders, sandbox.read,
// kReadableHomePaths, and the folders the environment points a command at --
// its PATH, and a variable naming a folder (GOPATH, ANDROID_HOME, ...), where
// a bin folder brings its parent, which holds the rest of its tool.
void CollectReads(SandboxInputs* inputs) {
  namespace fs = std::filesystem;
  auto canonical = [](const std::string& path) {
    return path.empty() ? std::string() : CanonicalAccessPath(path).string();
  };
  const std::string home = canonical(UserHome());
  if (home.empty()) return;
  const std::string around = fs::path(CanonicalCwd()).parent_path().string();
  for (const std::string& folder : {home, UagentDir(kSpacesDir), around}) {
    // The root's entries are the system's.
    if (folder == "/") continue;
    std::error_code error;
    for (fs::directory_iterator entry(folder, error), end;
         !error && entry != end; entry.increment(error)) {
      // A neighbour that holds the home folder, uAgent's state and the tools'
      // config with it, is closed only by the home folder's own entries.
      const std::string path = canonical(entry->path().string());
      if (!entry->path().filename().string().starts_with(".") &&
          !SandboxPathWithin(home, path)) {
        inputs->closed_folders.push_back(path);
      }
    }
  }
  std::vector<std::string> open;
  for (const fs::path& skills : SkillSearchPath(CanonicalCwd())) {
    open.push_back(canonical(skills.string()));
  }
  for (std::string_view path : kReadableHomePaths) {
    open.push_back(canonical(home + "/" + std::string(path)));
  }
  for (const std::string& raw :
       SplitPathList(StringSetting(Cfg("UAGENT_SANDBOX_READ")))) {
    open.push_back(canonical(ExpandHome(raw)));
  }
  for (char** entry = ProcessEnvironment(); entry && *entry; ++entry) {
    const std::string_view text(*entry);
    const size_t equals = text.find('=');
    const std::string_view key = text.substr(0, equals);
    if (equals == std::string_view::npos || key == "PWD" || key == "OLDPWD" ||
        key == "HOME") {
      continue;
    }
    for (const std::string& raw :
         SplitPathList(std::string(text.substr(equals + 1)))) {
      std::error_code missing;
      if (raw.empty() || raw.front() != '/' ||
          !fs::is_directory(raw, missing)) {
        continue;
      }
      fs::path folder = canonical(raw);
      // A bin folder brings its parent, which holds the rest of its tool --
      // unless that is the home folder, whose bin stands alone.
      if (folder.filename() == "bin" && folder.parent_path() != home) {
        folder = folder.parent_path();
      }
      open.push_back(folder.string());
    }
  }
  // Go's default GOPATH, when nothing names another.
  if (EnvStr("GOPATH").empty()) {
    std::error_code missing;
    if (fs::is_directory(home + "/go", missing)) open.push_back(home + "/go");
  }
  // A linked worktree's git directories hold what git reads there.
  const GitWorktree worktree = FindGitWorktree(inputs->workspace);
  for (const std::string& dir : {worktree.git_dir, worktree.common_dir}) {
    if (!dir.empty()) open.push_back(dir);
  }
  // The root (the parent of /bin) or the home folder would open every entry.
  for (std::string& path : open) {
    if (!path.empty() && path != "/" && path != home) {
      inputs->read_roots.push_back(std::move(path));
    }
  }
}

// The relaxed level: the home folder writable around ~/.uagent, and none of
// your folders closed to reads.
void Relax(SandboxInputs* inputs) {
  const std::string home = UserHome();
  if (!home.empty()) inputs->open_home = CanonicalAccessPath(home).string();
  inputs->closed_folders.clear();
  inputs->read_roots.clear();
}

// Every configured value the sandbox reads, resolved to the canonical absolute
// paths seatbelt and Landlock both match on. macOS makes this mandatory rather
// than tidy: /tmp there is a symlink to /private/tmp, and a profile naming the
// symlink confines nothing.
SandboxInputs CollectInputs(SandboxPreset preset) {
  auto canonical = [](const std::string& path) {
    return path.empty() ? std::string() : CanonicalAccessPath(path).string();
  };
  SandboxInputs inputs;
  // Outbound TCP is allowed unless sandbox.network says otherwise: git, npm
  // and pip all need it.
  inputs.read_only = preset == SandboxPreset::kReadOnly;
  inputs.allow_network =
      !inputs.read_only && BoolSetting(Cfg("UAGENT_SANDBOX_NET"));
  inputs.workspace = CanonicalCwd();
  std::error_code ec;
  inputs.git_repository =
      std::filesystem::is_directory(inputs.workspace + "/.git", ec);
  inputs.global_base = canonical(GlobalBase());
  inputs.tmpdir = canonical(EnvStr("TMPDIR"));
#if defined(__APPLE__)
  if (char temp[PATH_MAX];
      confstr(_CS_DARWIN_USER_TEMP_DIR, temp, sizeof temp) > 0) {
    inputs.user_tmpdir = canonical(temp);
  }
#endif
  inputs.terminal_logs = canonical(UagentDir(kTerminalLogsDir));
  // Canonicalised component by component rather than passed through raw: a
  // root written as ~/.uagent/.. is not textually inside ~/.uagent, so the
  // ancestor screen would let it past and both mechanisms would then resolve
  // it back to the home directory.
  for (const std::string& raw :
       SplitPathList(StringSetting(Cfg("UAGENT_SANDBOX_WRITE")))) {
    std::string root = canonical(raw);
    if (!root.empty()) inputs.extra_roots.push_back(std::move(root));
  }
  const std::string home = UserHome();
  if (!home.empty()) {
    // A root that does not exist grants nothing, and the tool could not then
    // make its own folder beneath a closed one.
    for (std::string_view cache : kPackageCaches) {
      const std::string path = home + "/" + std::string(cache);
      std::error_code error;
      std::filesystem::create_directories(path, error);
      inputs.tool_caches.push_back(canonical(path));
    }
#if defined(__APPLE__)
    for (std::string_view cache : kMacPackageCaches) {
      inputs.tool_caches.push_back(canonical(home + "/" + std::string(cache)));
    }
#endif
  }
  // What a commit in a linked worktree writes of the repository: the
  // worktree's own git directory, and the shared objects, refs and logs. Not
  // the shared config or hooks, which later run commands in your own git.
  const GitWorktree worktree = FindGitWorktree(inputs.workspace);
  if (!worktree.git_dir.empty()) {
    inputs.git_roots = {worktree.git_dir, worktree.common_dir + "/objects",
                        worktree.common_dir + "/refs",
                        worktree.common_dir + "/logs"};
  }
  if (preset == SandboxPreset::kRelaxed) {
    Relax(&inputs);
  } else {
    CollectReads(&inputs);
  }
  return inputs;
}

// A delegated child runs under the policy of the session that started it,
// whole: composing its own from the configuration could add a root, lose an
// exclusion, or turn a refusal into a policy that runs commands. Anything
// unreadable refuses.
SandboxStatus InheritedStatus(const std::string& text) {
  SandboxStatus status;
  const json from = json::parse(text, nullptr, false);
  const int mode = JsonValue(from, "mode", -1);
  status.mode = SandboxMode::kRefused;
  status.reason =
      "the sandbox policy this session was started with is "
      "unreadable";
  if (mode < 0 || mode > static_cast<int>(SandboxMode::kRefused)) return status;
  status.mode = static_cast<SandboxMode>(mode);
  status.level = SandboxSupported();
  status.reason = JsonValue(from, "reason", "");
  status.preset = JsonValue(from, "preset", "");
  if (!SandboxPolicyFromJson(JsonValue(from, "policy", json()),
                             &status.policy)) {
    status.mode = SandboxMode::kRefused;
    status.reason =
        "the sandbox policy this session was started with is unreadable";
  }
  return status;
}

// The status at the configured level and, when yolo would raise that level,
// the inputs of the raised one: made from the configured level's at start, so
// it follows the configuration the session started with, not one changed
// since.
struct ConfiguredSandbox {
  SandboxStatus status;
  std::optional<SandboxInputs> raised;
};

ConfiguredSandbox BuildStatus() {
  ConfiguredSandbox configured;
  SandboxStatus& status = configured.status;
  // A child runs under its parent's status whatever its own approval mode.
  if (const std::string inherited = EnvStr("UAGENT_INTERNAL_SANDBOX");
      !inherited.empty()) {
    status = InheritedStatus(inherited);
    return configured;
  }
  // A level somebody wrote down that names no level -- a typo, or `strict`,
  // which is gone -- must not quietly become the default, which opens the
  // network a stricter level would have closed. A saved one is dropped by the
  // settings reader, which reports its key.
  SandboxPreset preset = SandboxPreset::kDefault;
  const std::string level = StringSetting(Cfg("UAGENT_SANDBOX"));
  const std::vector<std::string> dropped =
      ReadSettings(CanonicalCwd()).invalid_keys;
  if (!ParseSandboxPreset(level, &preset) ||
      std::find(dropped.begin(), dropped.end(), "sandbox.level") !=
          dropped.end()) {
    status.mode = SandboxMode::kRefused;
    status.reason =
        "sandbox.level is not a level (readonly, default, relaxed or off); "
        "to deny the network set sandbox.network=false with the default level";
    return configured;
  }
  status.preset = level;
  if (preset == SandboxPreset::kOff) return configured;
  status.level = SandboxSupported();
  // Test-only, and only ever stricter: it can make an enforceable host look
  // unenforceable, never the other way round.
  if (!EnvStr("UAGENT_INTERNAL_SANDBOX_UNAVAILABLE").empty()) {
    status.level = SandboxLevel::kUnavailable;
  }
  // A value here is a value somebody wrote down: the settings hold what was
  // saved, the environment or a flag supplied, and never the registry
  // defaults. That is the whole of the provenance the two unenforceable
  // tiers need.
  const bool requested = !SettingText(Cfg("UAGENT_SANDBOX")).empty();
  if (status.level == SandboxLevel::kUnavailable) {
    status.mode = requested ? SandboxMode::kRefused : SandboxMode::kDegraded;
    status.reason =
#if defined(__linux__)
        "this kernel has no Landlock support (needs 5.13 or newer)";
#else
        "this host has no usable sandbox mechanism";
#endif
    return configured;
  }
  SandboxInputs inputs = CollectInputs(preset);
  // Denying the network on a host that can only confine writes would be a
  // policy nobody is enforcing. Unlike the tier above this refuses whatever
  // asked for it: a default cannot ask for a network denial, so anything that
  // reaches here was written down by somebody.
  if (!inputs.allow_network &&
      status.level != SandboxLevel::kFilesystemAndNetwork) {
    status.mode = SandboxMode::kRefused;
    status.reason =
        "this kernel's Landlock cannot restrict the network (needs ABI 4)";
    return configured;
  }
  SandboxPolicyResult built = BuildSandboxPolicy(inputs);
  status.policy = std::move(built.policy);
  status.rejected = std::move(built.rejected);
  status.mode = SandboxMode::kEnforced;
  if (YoloSandboxPreset(preset) != preset) {
    Relax(&inputs);
    configured.raised = std::move(inputs);
  }
  return configured;
}

// The configured status raised to relaxed, for yolo. The same host and
// network, so nothing the configured one passed is asked again.
SandboxStatus RaisedStatus(const ConfiguredSandbox& configured) {
  SandboxStatus status = configured.status;
  if (!configured.raised) return status;
  SandboxPolicyResult built = BuildSandboxPolicy(*configured.raised);
  status.policy = std::move(built.policy);
  status.rejected = std::move(built.rejected);
  status.preset = "relaxed (yolo)";
  return status;
}

// What the summary line calls the thing doing the confining.
const char* MechanismName() {
#if defined(__APPLE__)
  return "seatbelt";
#elif defined(__linux__)
  return "landlock";
#else
  return "none";
#endif
}

}  // namespace

const SandboxStatus& SandboxRuntime() {
  static const ConfiguredSandbox kConfigured = BuildStatus();
  if (!kConfigured.raised || !ApprovalIsYolo()) return kConfigured.status;
  // Composed on first use, once and thread-safely, like the one above.
  static const SandboxStatus kRaised = RaisedStatus(kConfigured);
  return kRaised;
}

std::string SandboxInheritance() {
  const SandboxStatus& status = SandboxRuntime();
  return JsonDump({{"mode", static_cast<int>(status.mode)},
                   {"reason", status.reason},
                   {"preset", status.preset},
                   {"policy", SandboxPolicyJson(status.policy)}});
}

json SandboxDiagnosticJson() {
  const SandboxStatus& status = SandboxRuntime();
  switch (status.mode) {
    case SandboxMode::kOff:
      return {{"mode", "off"}, {"summary", "off"}};
    case SandboxMode::kDegraded:
      return {
          {"mode", "degraded"},
          {"reason", status.reason},
          {"summary", "degraded, commands run unconfined: " + status.reason}};
    case SandboxMode::kRefused:
      return {{"mode", "refused"},
              {"reason", status.reason},
              {"summary", "refusing every command: " + status.reason}};
    case SandboxMode::kEnforced:
      break;
  }
  // Landlock restricts TCP and nothing else, so calling it "no network" on
  // Linux would promise a UDP closure that is not there.
  std::string summary =
      std::string(MechanismName()) + ", " + status.preset + ": writes";
  if (!status.policy.allow_network) {
#if defined(__linux__)
    summary += " + no outbound tcp";
#else
    summary += " + no network";
#endif
  }
  return {{"mode", "enforced"},
          {"mechanism", MechanismName()},
          {"level", status.preset},
          {"network", status.policy.allow_network ? "allowed" : "denied"},
          {"roots", status.policy.writable_roots},
          {"readable", status.policy.readable},
          {"rejected", status.rejected},
          {"summary", std::move(summary)}};
}

std::string SandboxNote(const SandboxStatus& status) {
  SandboxPolicy policy = status.policy;
#if defined(__linux__)
  policy.hidden = HiddenPaths();
#endif
  std::string note = SandboxWritesNote(policy, CanonicalCwd());
#if defined(__linux__)
  // Landlock grants the home folder entry by entry, so what does not exist yet
  // has no rule to allow it.
  if (!policy.withheld.empty()) {
    note += "; a new file made directly in the home folder is not writable";
  }
#endif
  return note;
}

std::string SandboxWorkspaceRefusal() {
  const SandboxStatus& status = SandboxRuntime();
  if (status.mode != SandboxMode::kEnforced ||
      SandboxWritable(status.policy, CanonicalCwd())) {
    return {};
  }
  return SandboxNote(status);
}

std::vector<std::string> SandboxWrapperArgv(const SandboxStatus& status) {
  if (status.mode != SandboxMode::kEnforced) return {};
  SandboxPolicy policy = status.policy;
  policy.hidden = HiddenPaths();
#if defined(__APPLE__)
  std::string profile = SeatbeltProfile(policy);
  // An oversized profile renders empty rather than truncated, and a truncated
  // profile is a weaker one. No wrapper here would mean no confinement, so the
  // caller is told to refuse instead.
  if (profile.empty()) return {};
  return {"/usr/bin/sandbox-exec", "-p", std::move(profile)};
#elif defined(__linux__)
  // The trampoline re-execs *this* image, so it asks the kernel which image
  // that is instead of trusting argv[0]. argv[0] is recorded by Bootstrap and
  // is therefore empty in any process that does not run it -- the unit test
  // binary among them, whose every sandboxed spawn then tried to exec "" --
  // and even in the agent it can be a bare `uagent` resolved from PATH, which
  // is not a path exec can use. /proc/self/exe is neither.
  std::string self = SelfExecutablePath();
  if (self.empty()) return {};
  // One argv word has a kernel limit of its own, below the limit on all of
  // them; a policy past it is refused like an oversized profile.
  std::string word = JsonDump(SandboxPolicyJson(policy));
  if (word.size() > kSandboxPolicyArgBytes) return {};
  return {std::move(self), "--sandbox-child", std::move(word), "--"};
#else
  return {};
#endif
}

namespace {

// The paths HiddenPaths() considers, canonical and whether or not they exist
// yet. They follow settings that need a restart, so they are worked out again
// only when those, or the home folder, differ from last time.
struct HiddenCandidates {
  std::mutex mutex;
  std::string key UAGENT_GUARDED_BY(mutex);
  std::vector<std::string> paths UAGENT_GUARDED_BY(mutex);
};

std::vector<std::string> CandidateHiddenPaths() {
  static HiddenCandidates cache;
  const std::string home = UserHome();
  const std::string browser = SettingText(Cfg("UAGENT_BROWSER_DATA"));
  const std::string revealed = StringSetting(Cfg("UAGENT_SANDBOX_REVEAL"));
  const std::string hidden = StringSetting(Cfg("UAGENT_SANDBOX_HIDE"));
  const std::string key = home + '\n' + GlobalBase() + '\n' + browser + '\n' +
                          revealed + '\n' + hidden;
  std::lock_guard lock(cache.mutex);
  if (key == cache.key) return cache.paths;
  // A listed path may start with ~/. Relative paths name nothing: they would
  // resolve against whatever folder the session happens to be in.
  auto absolute = [](const std::string& raw) {
    const std::string path = ExpandHome(raw);
    return path.empty() || path.front() != '/'
               ? std::string()
               : CanonicalAccessPath(path).string();
  };
  // The browser's profile, and the web host's own state: its paired devices'
  // tokens would let a command act as the person at a browser. uAgent's
  // settings hold the API keys.
  std::vector<std::string> candidates = {browser, GlobalBase() + "/web",
                                         GlobalBase() + "/" + kConfigDir};
  if (!home.empty()) {
    for (std::string_view store : kHiddenCredentialStores) {
      candidates.push_back(home + "/" + std::string(store));
    }
  }
  std::vector<std::string> shown;
  for (const std::string& raw : SplitPathList(revealed)) {
    shown.push_back(absolute(raw));
  }
  std::vector<std::string> paths;
  auto add = [&](std::string path) {
    if (!path.empty() &&
        std::find(paths.begin(), paths.end(), path) == paths.end()) {
      paths.push_back(std::move(path));
    }
  };
  for (const std::string& path : candidates) {
    const std::string canonical = absolute(path);
    const bool lifted =
        std::any_of(shown.begin(), shown.end(), [&](const std::string& root) {
          return !root.empty() && SandboxPathWithin(canonical, root);
        });
    if (!lifted) add(canonical);
  }
  for (const std::string& raw : SplitPathList(hidden)) add(absolute(raw));
  cache.key = key;
  cache.paths = paths;
  return paths;
}

}  // namespace

std::vector<std::string> HiddenPaths() {
  // Existence is asked every time: a store made mid-session is hidden from the
  // next command.
  std::vector<std::string> hidden = CandidateHiddenPaths();
  std::erase_if(hidden, [](const std::string& path) {
    struct stat info{};
    return lstat(path.c_str(), &info) != 0;
  });
  return hidden;
}

std::vector<std::string> SandboxUnenforcedWrites(const SandboxStatus& status) {
#if defined(__linux__)
  return status.policy.denied_writes;
#else
  static_cast<void>(status);
  return {};
#endif
}

int SandboxChildMain(int argc, char** argv) {
#if defined(__linux__)
  // argv[0] is the binary, argv[1] the flag that routed us here, argv[2] the
  // policy and argv[3] the `--` before the command.
  SandboxPolicy policy;
  if (argc < 3 ||
      !SandboxPolicyFromJson(json::parse(argv[2], nullptr, false), &policy)) {
    fprintf(stderr, "uagent: sandbox: malformed policy\n");
    return 125;
  }
  if (argc < 5 || std::string_view(argv[3]) != "--") {
    fprintf(stderr, "uagent: sandbox: missing command\n");
    return 125;
  }
  if (!ApplyLandlock(policy)) return 125;
  execvp(argv[4], argv + 4);
  fprintf(stderr, "uagent: sandbox: exec %s failed (errno %d)\n", argv[4],
          errno);
  return errno == ENOENT ? 127 : 126;
#else
  static_cast<void>(argc);
  static_cast<void>(argv);
  fprintf(stderr, "uagent: sandbox: no trampoline on this platform\n");
  return 125;
#endif
}

}  // namespace uagent
