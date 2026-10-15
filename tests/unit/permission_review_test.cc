// Copyright 2026 Timon Gentzsch

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "include/app/permissions.h"
#include "include/core/runtime_config.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

// The answers a decisions request returns: P(allow), P(deny) and one
// probability for each of the four reason questions.
json Answers(double allow, double deny, double destructive = 0,
             double beyond = 0, double sends = 0, double secrets = 0) {
  auto noul = [](double probability) {
    return json{{"type", "noul"}, {"noul", probability}};
  };
  return {{"permission",
           {{"type", "choice"},
            {"probabilities",
             {{"allow", allow}, {"ask", 1 - allow - deny}, {"deny", deny}}}}},
          {"destructive", noul(destructive)},
          {"beyond_request", noul(beyond)},
          {"sends_data_off_machine", noul(sends)},
          {"touches_secrets", noul(secrets)}};
}

Endpoint OpenRouterEndpoint() {
  Endpoint endpoint;
  endpoint.base_url = "https://openrouter.ai/api/v1";
  endpoint.api_key = "key";
  CHECK(DeclareEndpoint(endpoint, {}));
  return endpoint;
}

Endpoint Serving(const std::string& name, const std::string& key) {
  Endpoint endpoint;
  endpoint.name = name;
  endpoint.base_url = "https://" + name + ".test/v1";
  endpoint.api_key = key;
  endpoint.features = FeaturesOf({Feature::kDecisions});
  endpoint.options = {{"decisions_url", "https://" + name + ".test/decide"},
                      {"decisions_model", name + "-model"}};
  return endpoint;
}

}  // namespace

void TestPermissionReview() {
  // The questions are the ones tests/reviewer_eval measures the bars with.
  {
    std::ifstream file(std::string(UAGENT_TEST_SOURCE_DIR) +
                       "/tests/reviewer_eval/questions.json");
    std::stringstream text;
    text << file.rdbuf();
    CHECK(json::parse(text.str()) == ReviewQuestions());
  }

  // The bars decide at their edges: allow at or above, deny at or above, and
  // a reason at or above its bar asks.
  const ReviewBars bars{0.6, 0.95, 0.7};
  using Decision = AutoPermissionDecision;
  CHECK(JudgeAnswers(Answers(0.6, 0), bars).decision == Decision::kAllow);
  CHECK(JudgeAnswers(Answers(0.59, 0), bars).decision == Decision::kAsk);
  CHECK(JudgeAnswers(Answers(0.02, 0.95), bars).decision == Decision::kDeny);
  CHECK(JudgeAnswers(Answers(0.02, 0.94), bars).decision == Decision::kAsk);
  CHECK(JudgeAnswers(Answers(0.9, 0, 0, 0, 0.69), bars).decision ==
        Decision::kAllow);
  AutoPermissionReview vetoed = JudgeAnswers(Answers(0.99, 0, 0, 0, 0.7), bars);
  CHECK(vetoed.decision == Decision::kAsk);
  CHECK(vetoed.error.empty());
  CHECK(vetoed.probabilities["allow"] == 0.99);
  // The reason that forced the question is on the card, by name and
  // probability, with the bar it reached.
  CHECK(vetoed.reasons.size() == 1);
  CHECK(vetoed.reasons[0]["id"] == "sends_data_off_machine");
  CHECK(vetoed.reasons[0]["label"] == "sends data off this machine 70%");
  CHECK(vetoed.note.find("reaches the 70% that asks") != std::string::npos);
  // A likely reason below the veto is shown but does not decide.
  AutoPermissionReview shown = JudgeAnswers(Answers(0.9, 0, 0.5), bars);
  CHECK(shown.decision == Decision::kAllow);
  CHECK(shown.reasons.size() == 1);
  CHECK(JudgeAnswers(Answers(0.9, 0, 0.49), bars).reasons.empty());
  // The most likely vetoing reason is the one named.
  CHECK(JudgeAnswers(Answers(0.99, 0, 0.8, 0.95), bars)
            .note.starts_with("beyond what you asked 95%"));
  // A reviewer that does not answer every question has not reviewed.
  json missing = Answers(0.99, 0);
  missing.erase("touches_secrets");
  AutoPermissionReview unanswered = JudgeAnswers(missing, bars);
  CHECK(unanswered.decision == Decision::kAsk);
  CHECK(unanswered.error.find("touches_secrets") != std::string::npos);
  CHECK(!JudgeAnswers(json::object(), bars).error.empty());
  json argmax_only = Answers(0, 0);
  argmax_only["permission"] = {{"type", "choice"}, {"choice", "allow"}};
  AutoPermissionReview undecided = JudgeAnswers(argmax_only, bars);
  CHECK(undecided.decision == Decision::kAsk);
  CHECK(!undecided.error.empty());

  // A model's bars are its row of the endpoint's table, then the settings.
  const Endpoint openrouter = OpenRouterEndpoint();
  const RuntimeConfig defaults = RuntimeConfig::FromValues({});
  ReviewBars jev = ReviewBarsFor(openrouter, "~typesafe/jev-latest", defaults);
  CHECK(jev.allow == 0.6 && jev.deny == 0.95 && jev.veto == 0.7);
  ReviewBars luna =
      ReviewBarsFor(openrouter, "openai/gpt-6-luna-decisions", defaults);
  CHECK(luna.allow == 0.85 && luna.veto == 0.9);
  // A model nobody calibrated is held to the strict bars.
  ReviewBars unknown = ReviewBarsFor(openrouter, "other/model", defaults);
  CHECK(unknown.allow == 0.9 && unknown.deny == 0.95 && unknown.veto == 0.3);
  const RuntimeConfig tuned =
      RuntimeConfig::FromValues({{"UAGENT_PERMISSION_ALLOW_ABOVE", "0.97"},
                                 {"UAGENT_PERMISSION_DENY_ABOVE", "wat"},
                                 {"UAGENT_PERMISSION_VETO_ABOVE", "2"}});
  ReviewBars overridden =
      ReviewBarsFor(openrouter, "~typesafe/jev-latest", tuned);
  CHECK(overridden.allow == 0.97);
  CHECK(overridden.deny == 0.95 && overridden.veto == 0.7);

  // The review asks an endpoint that serves decision models, and says so when
  // there is none: nothing else stands in.
  Api api(RuntimeConfig{});
  const Endpoint first = Serving("first", "k1");
  const Endpoint second = Serving("second", "k2");
  RouteResolution none = ResolveReviewRoute(api, {}, "");
  CHECK(!none.none.empty());
  CHECK(!ResolveReviewRoute(api, {Serving("keyless", "")}, "").none.empty());
  RouteResolution found = ResolveReviewRoute(api, {first, second}, "");
  CHECK(found.none.empty());
  CHECK(found.route.name == "first" && found.route.model == "first-model");
  // The setting names the model, optionally on an endpoint.
  found = ResolveReviewRoute(api, {first, second}, "second/judge-1");
  CHECK(found.route.name == "second" && found.route.model == "judge-1");
  found = ResolveReviewRoute(api, {first, second}, "vendor/judge-2");
  CHECK(found.route.name == "first" && found.route.model == "vendor/judge-2");
  // The conversation's own endpoint comes first when it serves them.
  static_cast<Endpoint&>(api) = second;
  CHECK(ResolveReviewRoute(api, {first}, "").route.name == "second");

  // A path argument outside the folder is what the model never judges.
  const std::string root = CanonicalCwd();
  CHECK(!PathOutsideFolder({{"command", "ls"}}, root));
  CHECK(!PathOutsideFolder({{"path", "a.txt"}}, root));
  CHECK(PathOutsideFolder({{"path", "/etc/hosts"}}, root));
}

}  // namespace uagent
