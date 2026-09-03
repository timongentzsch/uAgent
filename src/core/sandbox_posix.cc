// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"

// Everything that has to ask the host what it can enforce, and the Linux
// trampoline that enforces it. The platform split lives inside this file so
// that sandbox.h stays ifdef-free and sandbox.cc stays pure.

namespace uagent {
namespace {

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
// unrestricted as the macOS profile's (allow default) leaves them. It also
// keeps every rule below grantable, since allowed_access must be a subset of
// what the ruleset handles.
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

bool ApplyLandlock(const SandboxPolicy& policy) {
  const int abi = LandlockAbi();
  if (abi < 0) return Fatal("landlock probe");
  if (!policy.allow_network && abi < 4) return Fatal("network confinement");

  RulesetAttr attr{};
  attr.handled_access_fs = HandledFsAccess(abi);
  if (!policy.allow_network) {
    attr.handled_access_net = kAccessBindTcp | kAccessConnectTcp;
  }
  // An older kernel rejects a size it does not recognise, so send the prefix
  // it knows: the net field only exists from ABI 4 on.
  const size_t attr_size =
      abi >= 4 ? sizeof(attr) : sizeof(attr.handled_access_fs);
  Fd ruleset(static_cast<int>(syscall(kCreateRuleset, &attr, attr_size, 0U)));
  if (!ruleset) return Fatal("landlock_create_ruleset");

  for (const std::string& root : policy.writable_roots) {
    Fd parent(open(root.c_str(), O_PATH | O_CLOEXEC));
    // A root that does not exist grants nothing, which is stricter than the
    // policy asked for and so cannot open a hole. Skipping it keeps optional
    // roots -- an unset $TMPDIR, a cache dir nobody has created yet -- from
    // turning into a refusal to run.
    if (!parent) continue;
    PathBeneathAttr rule{};
    rule.allowed_access = attr.handled_access_fs;
    rule.parent_fd = parent.Get();
    if (syscall(kAddRule, ruleset.Get(), kRulePathBeneath, &rule, 0U) != 0) {
      return Fatal("landlock_add_rule");
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

// Every configured value the sandbox reads, resolved to the canonical absolute
// paths seatbelt and Landlock both match on. macOS makes this mandatory rather
// than tidy: /tmp there is a symlink to /private/tmp, and a profile naming the
// symlink confines nothing.
SandboxInputs CollectInputs() {
  auto canonical = [](const std::string& path) {
    return path.empty() ? std::string() : CanonicalAccessPath(path).string();
  };
  SandboxInputs inputs;
  inputs.allow_network = SandboxNetworkAllowed();
  inputs.workspace = CanonicalCwd();
  inputs.global_base = canonical(GlobalBase());
  inputs.tmpdir = canonical(EnvStr("TMPDIR"));
  inputs.terminal_logs = canonical(UagentDir(kTerminalLogsDir));
  // Canonicalised component by component rather than passed through raw: a
  // root written as ~/.uagent/.. is not textually inside ~/.uagent, so the
  // ancestor screen would let it past and both mechanisms would then resolve
  // it back to the home directory.
  const std::string raw = SandboxWriteRoots();
  for (size_t start = 0; start < raw.size();) {
    size_t end = raw.find(':', start);
    if (end == std::string::npos) end = raw.size();
    std::string root = canonical(raw.substr(start, end - start));
    if (!root.empty()) {
      if (!inputs.extra_roots.empty()) inputs.extra_roots += ':';
      inputs.extra_roots += root;
    }
    start = end + 1;
  }
  const std::string home = UserHome();
  if (!home.empty()) {
    inputs.tool_caches = {canonical(home + "/.cache"),
                          canonical(home + "/.local/share")};
#if defined(__APPLE__)
    inputs.tool_caches.push_back(canonical(home + "/Library/Caches"));
#endif
  }
  return inputs;
}

SandboxStatus BuildStatus() {
  SandboxStatus status;
  if (!SandboxEnabled()) return status;
  status.level = SandboxSupported();
  // Test-only, and only ever stricter: it can make an enforceable host look
  // unenforceable, never the other way round.
  if (!EnvStr("UAGENT_INTERNAL_SANDBOX_UNAVAILABLE").empty()) {
    status.level = SandboxLevel::kUnavailable;
  }
  // A value in the environment here is a value somebody wrote down: the
  // configuration layer exports what a file, the environment or a flag
  // supplied, and never the registry defaults. That is the whole of the
  // provenance the two unenforceable tiers need.
  const bool requested = !EnvStr("UAGENT_SANDBOX").empty();
  if (status.level == SandboxLevel::kUnavailable) {
    status.mode = requested ? SandboxMode::kRefused : SandboxMode::kDegraded;
    status.reason =
#if defined(__linux__)
        "this kernel has no Landlock support (needs 5.13 or newer)";
#else
        "this host has no usable sandbox mechanism";
#endif
    return status;
  }
  SandboxInputs inputs = CollectInputs();
  // Denying the network on a host that can only confine writes would be a
  // policy nobody is enforcing. Unlike the tier above this refuses whatever
  // asked for it: a default cannot ask for a network denial, so anything that
  // reaches here was written down by somebody.
  if (!inputs.allow_network &&
      status.level != SandboxLevel::kFilesystemAndNetwork) {
    status.mode = SandboxMode::kRefused;
    status.reason =
        "this kernel's Landlock cannot restrict the network (needs ABI 4)";
    return status;
  }
  SandboxPolicyResult built = BuildSandboxPolicy(inputs);
  status.policy = std::move(built.policy);
  status.rejected = std::move(built.rejected);
  status.mode = SandboxMode::kEnforced;
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
  static const SandboxStatus kStatus = BuildStatus();
  return kStatus;
}

json SandboxDiagnosticJson() {
  if (ApprovalIsAutomatic()) {
    return {{"mode", "off"},
            {"reason", "yolo approval mode"},
            {"summary", "off (yolo)"}};
  }
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
  std::string summary = std::string(MechanismName()) + ": writes";
  if (!status.policy.allow_network) {
#if defined(__linux__)
    summary += " + no outbound tcp";
#else
    summary += " + no network";
#endif
  }
  return {{"mode", "enforced"},
          {"mechanism", MechanismName()},
          {"network", status.policy.allow_network ? "allowed" : "denied"},
          {"roots", status.policy.writable_roots},
          {"rejected", status.rejected},
          {"summary", std::move(summary)}};
}

std::vector<std::string> SandboxWrapperArgv(const SandboxStatus& status) {
  if (status.mode != SandboxMode::kEnforced) return {};
#if defined(__APPLE__)
  std::string profile = SeatbeltProfile(status.policy);
  // An oversized profile renders empty rather than truncated, and a truncated
  // profile is a weaker one. No wrapper here would mean no confinement, so the
  // caller is told to refuse instead.
  if (profile.empty()) return {};
  return {"/usr/bin/sandbox-exec", "-p", std::move(profile)};
#elif defined(__linux__)
  std::vector<std::string> argv{ExecutablePath(), "--sandbox-child"};
  std::vector<std::string> words = EncodeSandboxPolicy(status.policy);
  argv.insert(argv.end(), std::make_move_iterator(words.begin()),
              std::make_move_iterator(words.end()));
  argv.emplace_back("--");
  return argv;
#else
  return {};
#endif
}

int SandboxChildMain(int argc, char** argv) {
#if defined(__linux__)
  // argv[0] is the binary and argv[1] the flag that routed us here; the policy
  // words start after them.
  std::vector<std::string> words;
  for (int i = 2; i < argc; ++i) words.emplace_back(argv[i]);
  SandboxPolicy policy;
  size_t consumed = 0;
  if (!DecodeSandboxPolicy(words, &policy, &consumed)) {
    fprintf(stderr, "uagent: sandbox: malformed policy\n");
    return 125;
  }
  if (consumed >= words.size() || words[consumed] != "--") {
    fprintf(stderr, "uagent: sandbox: missing command\n");
    return 125;
  }
  const int first = 2 + static_cast<int>(consumed) + 1;
  if (first >= argc) {
    fprintf(stderr, "uagent: sandbox: missing command\n");
    return 125;
  }
  if (!ApplyLandlock(policy)) return 125;
  execvp(argv[first], argv + first);
  fprintf(stderr, "uagent: sandbox: exec %s failed (errno %d)\n", argv[first],
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
