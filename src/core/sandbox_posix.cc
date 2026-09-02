// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include "include/core/fd.h"
#include "include/core/sandbox.h"

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
