// Copyright 2026 Timon Gentzsch

#include "include/core/sandbox.h"

#include <algorithm>
#include <string>
#include <vector>

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
  inputs.cache_dir = "/home/u/.cache";
  inputs.data_dir = "/home/u/.local/share";
  inputs.terminal_logs = "/home/u/.uagent/terminals/logs";
  return inputs;
}

}  // namespace

void TestSandboxPolicy() {
  SandboxPolicyResult base = BuildSandboxPolicy(BaseInputs());
  CHECK(HasRoot(base.policy, "/home/u/work"));
  CHECK(HasRoot(base.policy, "/private/var/folders/ab/T"));
  CHECK(HasRoot(base.policy, "/home/u/.cache"));
  CHECK(HasRoot(base.policy, "/home/u/.local/share"));
  // The detached log directory is the one deliberate hole inside ~/.uagent.
  CHECK(HasRoot(base.policy, "/home/u/.uagent/terminals/logs"));
  // /dev is granted on both platforms, not just Linux: under a blanket write
  // deny, `2>/dev/null` fails, and that is most shell commands.
  CHECK(HasRoot(base.policy, "/dev"));
  // The project's own config sits inside the writable workspace, so it is
  // carved back out by path rather than by leaving the workspace ungranted.
  const std::vector<std::string> project_authority = {
      "/home/u/work/.uagent/.config", "/home/u/work/.mcp.json"};
  CHECK(base.policy.denied_writes == project_authority);
  CHECK(base.rejected.empty());
  CHECK(base.policy.allow_network);

  // The regression this whole design exists for: no root may be ~/.uagent or
  // an ancestor of it, or the config and the trust store come along with it.
  SandboxInputs reaching = BaseInputs();
  reaching.extra_roots = "/home/u/.uagent:/home/u:/:/home/u/.uagent/..";
  SandboxPolicyResult guarded = BuildSandboxPolicy(reaching);
  for (const std::string& root : guarded.policy.writable_roots) {
    CHECK(!SandboxPathWithin("/home/u/.uagent", root));
  }
  CHECK(Rejected(guarded, "/home/u/.uagent"));
  CHECK(Rejected(guarded, "/home/u"));
  CHECK(Rejected(guarded, "/"));

  // Relative roots are rejected rather than resolved: this code never touches
  // the filesystem, so it cannot know what one would resolve to.
  SandboxInputs relative = BaseInputs();
  relative.extra_roots = "build:./out:/opt/ok";
  SandboxPolicyResult filtered = BuildSandboxPolicy(relative);
  CHECK(Rejected(filtered, "build"));
  CHECK(Rejected(filtered, "./out"));
  CHECK(HasRoot(filtered.policy, "/opt/ok"));

  // A root nested in another is folded away, and duplicates with it, so the
  // profile is a function of the set rather than of discovery order.
  SandboxInputs nested = BaseInputs();
  nested.extra_roots = "/home/u/work/sub:/home/u/work:/opt/a:/opt/a";
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

void TestSandboxTrampolineArgs() {
  SandboxPolicy policy;
  policy.writable_roots = {"/w", "/private/tmp"};
  policy.allow_network = false;
  std::vector<std::string> words = EncodeSandboxPolicy(policy);
  // The command follows the policy words, so a decoder that miscounts would
  // silently confine the wrong argv.
  words.emplace_back("--");
  words.emplace_back("/bin/sh");

  SandboxPolicy decoded;
  size_t consumed = 0;
  REQUIRE(DecodeSandboxPolicy(words, &decoded, &consumed));
  CHECK(consumed == 4);
  CHECK(words[consumed] == "--");
  CHECK(decoded.writable_roots == policy.writable_roots);
  CHECK(!decoded.allow_network);

  SandboxPolicy allowed;
  allowed.allow_network = true;
  SandboxPolicy round;
  REQUIRE(DecodeSandboxPolicy(EncodeSandboxPolicy(allowed), &round, &consumed));
  CHECK(round.allow_network);
  CHECK(round.writable_roots.empty());
  CHECK(consumed == 2);

  // Every malformed form has to fail rather than decode to an empty policy,
  // which the trampoline would enforce as no confinement at all.
  SandboxPolicy ignored;
  CHECK(!DecodeSandboxPolicy({}, &ignored, &consumed));
  CHECK(!DecodeSandboxPolicy({"net=1"}, &ignored, &consumed));
  CHECK(!DecodeSandboxPolicy({"net=maybe", "roots=0"}, &ignored, &consumed));
  CHECK(!DecodeSandboxPolicy({"net=1", "roots="}, &ignored, &consumed));
  CHECK(!DecodeSandboxPolicy({"net=1", "roots=x"}, &ignored, &consumed));
  CHECK(!DecodeSandboxPolicy({"net=1", "count=1", "/w"}, &ignored, &consumed));
  CHECK(!DecodeSandboxPolicy({"net=1", "roots=2", "/w"}, &ignored, &consumed));
  CHECK(!DecodeSandboxPolicy({"net=1", "roots=99999999999999999999", "/w"},
                             &ignored, &consumed));
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
