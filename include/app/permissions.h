// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_PERMISSIONS_H_
#define UAGENT_INCLUDE_APP_PERMISSIONS_H_
// Remembered approvals: the key a call is remembered under, what approving
// it risks, and the rules kept for this conversation and for a folder.

#include <string>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

// A remembered approval: the calls of `tool` whose subject `match` covers.
// A match is "*" (every call), "PREFIX *" (one simple command starting with
// PREFIX), "DIR/" (a path under DIR) or the subject itself.
struct PermissionRule {
  std::string tool, match;
};

// What a rule is matched against: a command, ./path inside the folder `root`
// (else its absolute path), a URL's host, or the call's arguments.
std::string RuleSubject(const Tool& tool, const json& arguments,
                        const std::string& root);
// The broadest rule a person is offered for this call. A command's prefix is
// offered, and covers, only where it runs `confined` in the enforced sandbox.
PermissionRule SuggestRule(const Tool& tool, const json& arguments,
                           const std::string& root, bool confined);
bool RuleCovers(const PermissionRule& rule, const Tool& tool,
                const json& arguments, const std::string& root, bool confined);
// The rule in words: "run npm test *", "every mcp_x call".
std::string RuleLabel(const PermissionRule& rule);

// Whether the call names a path outside the folder `root`.
bool PathOutsideFolder(const json& arguments, const std::string& root);

// [{id, label}] of runs, writes, network, outside; most serious first.
json ApprovalRisks(const Tool& tool, const json& arguments,
                   const std::string& root);

// Whether a rule kept for `root` covers the call; `matched` takes it.
bool RepositoryRuleAllows(const std::string& root, const Tool& tool,
                          const json& arguments, bool confined,
                          PermissionRule* matched = nullptr);
bool RememberRepositoryRule(const std::string& root, const PermissionRule& rule,
                            std::string& error);
json PermissionRulesControl(const json& request);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_PERMISSIONS_H_
