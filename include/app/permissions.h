// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_PERMISSIONS_H_
#define UAGENT_INCLUDE_APP_PERMISSIONS_H_
// Remembered approvals: the key a call is remembered under, what approving
// it risks, and the rules kept for this conversation and for a folder.

#include <optional>
#include <string>
#include <string_view>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// What an approval is answered with, by a person or by a parent agent.
inline constexpr std::string_view kAllowOnce = "y";
inline constexpr std::string_view kAllowSession = "s";
inline constexpr std::string_view kAllowFolder = "a";
inline constexpr std::string_view kDeny = "n";
inline constexpr std::string_view kDenyFolder = "x";

// What a rule does to the calls it covers: deny beats ask, ask beats allow.
enum class RuleEffect { kAllow, kAsk, kDeny };
const char* RuleEffectName(RuleEffect effect);
bool ParseRuleEffect(std::string_view name, RuleEffect& effect);

// A remembered rule: the calls of `tool` whose subject `match` covers.
// A match is "*" (every call), "PREFIX *" (a command starting with PREFIX),
// "DIR/" (a path under DIR) or the subject itself.
struct PermissionRule {
  std::string tool, match;
  RuleEffect effect = RuleEffect::kAllow;
};

// What a rule is matched against: a command, ./path inside the folder `root`
// (else its absolute path), a URL's host, or the call's arguments.
std::string RuleSubject(const Tool& tool, const json& arguments,
                        const std::string& root);
// The broadest rule a person is offered for this call. A command's prefix is
// offered, and allows, only where it runs `confined` in the enforced sandbox.
PermissionRule SuggestRule(const Tool& tool, const json& arguments,
                           const std::string& root, bool confined);
// An allow rule covers one simple command; a deny or ask rule covers a
// command when any part of it, between `&&`, `||`, `;`, `|`, `&` or a line
// break, matches, sandbox or not.
bool RuleCovers(const PermissionRule& rule, const Tool& tool,
                const json& arguments, const std::string& root, bool confined);
// The rule in words: "run npm test *", "every mcp_x call".
std::string RuleLabel(const PermissionRule& rule);

// Whether the call names a path outside the folder `root`.
bool PathOutsideFolder(const json& arguments, const std::string& root);

// [{id, label}] of runs, writes, network, outside; most serious first.
json ApprovalRisks(const Tool& tool, const json& arguments,
                   const std::string& root);

// The rules kept for `root` that cover the call, one of each effect at most.
// `error` says why the rules file could not be read; nothing covers then.
struct CoveringRules {
  std::optional<PermissionRule> deny, ask, allow;
  std::string error;
};
CoveringRules FolderRules(const std::string& root, const Tool& tool,
                          const json& arguments, bool confined);
// Keeps `rule` for `root`, in place of one with the same tool and match.
bool RememberRepositoryRule(const std::string& root, const PermissionRule& rule,
                            std::string& error);
json PermissionRulesControl(const json& request);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_PERMISSIONS_H_
