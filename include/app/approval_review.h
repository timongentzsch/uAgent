// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_APPROVAL_REVIEW_H_
#define UAGENT_INCLUDE_APP_APPROVAL_REVIEW_H_
// The decision model of Auto: what it is asked, and what its answers decide
// under a model's bars.

#include <cstdint>
#include <string>

#include "include/api/client.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"

namespace uagent {

enum class AutoPermissionDecision { kAllow, kAsk, kDeny };

// Allow needs P(allow) >= `allow` and every reason question below `veto`;
// deny needs P(deny) >= `deny`; anything else asks.
struct ReviewBars {
  double allow, deny, veto;
};

struct AutoPermissionReview {
  AutoPermissionDecision decision = AutoPermissionDecision::kAsk;
  json probabilities = json::object();
  // [{id, label}], each label with its probability; `note` names the bar.
  json reasons = json::array();
  std::string note;
  std::string error;
};

// The questions every review asks, as tests/reviewer_eval/questions.json.
const json& ReviewQuestions();

// The `decision_bars` row for `model` at `endpoint`, else the strict bars.
ReviewBars ReviewBarsFor(const Endpoint& endpoint, const std::string& model);

// The verdict the answers of a decisions request give under `bars`.
AutoPermissionReview JudgeAnswers(const json& answers, const ReviewBars& bars);

struct ReviewedCall {
  const Tool& tool;
  std::string preview, user_request, session_id;
  int64_t turn = 0;
};

// Asks the decision model `route` names; with no endpoint for one, it asks.
AutoPermissionReview ReviewPermission(Api& api, const RouteResolution& route,
                                      UsageAccumulator& usage,
                                      const ReviewedCall& call);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_APPROVAL_REVIEW_H_
