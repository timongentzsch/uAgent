// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_PERMISSIONS_H_
#define UAGENT_INCLUDE_APP_PERMISSIONS_H_
// Approval policy: the key a call is remembered under, what approving it
// risks, remembered rules and the automatic review's decisions.

#include <cstdint>
#include <string>
#include <vector>

#include "include/api/client.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"

namespace uagent {

struct RuntimeConfig;

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
// The broadest rule a person is offered for this call: a command and its
// subcommand, the file's folder, the host, an MCP tool, or the call itself.
// A command's prefix is offered, and covers, only where it runs `confined` in
// the enforced sandbox, which holds whatever the prefix lets through.
PermissionRule SuggestRule(const Tool& tool, const json& arguments,
                           const std::string& root, bool confined);
bool RuleCovers(const PermissionRule& rule, const Tool& tool,
                const json& arguments, const std::string& root, bool confined);
// The rule in words: "run npm test *", "every mcp_x call".
std::string RuleLabel(const PermissionRule& rule);

// Whether the call names a path outside the folder `root`.
bool PathOutsideFolder(const json& arguments, const std::string& root);

// What approving this call risks, for the headline of its approval: runs,
// writes, network, outside (the folder `root`). [{id, label}], most serious
// first; empty for a call that only reads.
json ApprovalRisks(const Tool& tool, const json& arguments,
                   const std::string& root);

// Whether a rule kept for `root` covers the call; `matched` takes it.
bool RepositoryRuleAllows(const std::string& root, const Tool& tool,
                          const json& arguments, bool confined,
                          PermissionRule* matched = nullptr);
bool RememberRepositoryRule(const std::string& root, const PermissionRule& rule,
                            std::string& error);
json PermissionRulesControl(const json& request);

enum class AutoPermissionDecision { kAllow, kAsk, kDeny };

// The probabilities that decide a review: allow needs P(allow) of at least
// `allow` and every reason question below `veto`; deny needs P(deny) of at
// least `deny`; anything else asks.
struct ReviewBars {
  double allow, deny, veto;
};

struct AutoPermissionReview {
  AutoPermissionDecision decision = AutoPermissionDecision::kAsk;
  json probabilities = json::object();
  // The likely reasons for caution, [{id, label}] (the label carries the
  // probability), and in words which bar decided.
  json reasons = json::array();
  std::string note;
  std::string error;
};

// The questions every review asks, as tests/reviewer_eval/questions.json.
const json& ReviewQuestions();

// The bars for `model` at `endpoint`: its row of `decision_bars`, else the
// strict ones.
ReviewBars ReviewBarsFor(const Endpoint& endpoint, const std::string& model);

// The verdict the answers of a decisions request give under `bars`.
AutoPermissionReview JudgeAnswers(const json& answers, const ReviewBars& bars);

struct ReviewedCall {
  const Tool& tool;
  std::string preview, user_request, session_id;
  int64_t turn = 0;
};

// Asks the decision model `route` names; with no endpoint serving one the
// review says so and asks.
AutoPermissionReview ReviewPermission(Api& api, const RouteResolution& route,
                                      UsageAccumulator& usage,
                                      const ReviewedCall& call);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_PERMISSIONS_H_
