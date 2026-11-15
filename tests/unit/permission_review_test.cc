// Copyright 2026 Timon Gentzsch

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "include/app/permissions.h"
#include "include/core/runtime_config.h"
#include "include/core/sandbox.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

// The answers a decisions request returns, as probabilities.
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
  CHECK(DeclareHost(endpoint));
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

TEST_CASE(TestPermissionReview) {
  // The questions are the ones tests/reviewer_eval measures the bars with.
  {
    std::ifstream file(std::string(UAGENT_TEST_SOURCE_DIR) +
                       "/tests/reviewer_eval/questions.json");
    std::stringstream text;
    text << file.rdbuf();
    CHECK(json::parse(text.str()) == ReviewQuestions());
  }

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
  CHECK(vetoed.reasons.size() == 1);
  CHECK(vetoed.reasons[0]["id"] == "sends_data_off_machine");
  CHECK(vetoed.reasons[0]["label"] == "sends data off this machine 70%");
  CHECK(vetoed.note.find("reaches the 70% that asks") != std::string::npos);
  AutoPermissionReview shown = JudgeAnswers(Answers(0.9, 0, 0.5), bars);
  CHECK(shown.decision == Decision::kAllow);
  CHECK(shown.reasons.size() == 1);
  CHECK(JudgeAnswers(Answers(0.9, 0, 0.49), bars).reasons.empty());
  CHECK(JudgeAnswers(Answers(0.99, 0, 0.8, 0.95), bars)
            .note.starts_with("beyond what you asked 95%"));
  json missing = Answers(0.99, 0);
  missing.erase("touches_secrets");
  AutoPermissionReview unanswered = JudgeAnswers(missing, bars);
  CHECK(unanswered.decision == Decision::kAsk);
  CHECK(unanswered.error.find("touches_secrets") != std::string::npos);
  CHECK(!JudgeAnswers(json::object(), bars).error.empty());
  for (const double wrong : {2.0, -1.0}) {
    CHECK(JudgeAnswers(Answers(wrong, 0), bars).decision == Decision::kAsk);
    CHECK(!JudgeAnswers(Answers(wrong, 0), bars).error.empty());
    CHECK(!JudgeAnswers(Answers(0.9, 0, wrong), bars).error.empty());
  }
  json no_deny = Answers(0.99, 0);
  no_deny["permission"]["probabilities"].erase("deny");
  CHECK(JudgeAnswers(no_deny, bars).decision == Decision::kAsk);
  CHECK(!JudgeAnswers(no_deny, bars).error.empty());
  CHECK(JudgeAnswers(Answers(0.99, 0), {0.6, 0.95, 0}).decision ==
        Decision::kAsk);
  json argmax_only = Answers(0, 0);
  argmax_only["permission"] = {{"type", "choice"}, {"choice", "allow"}};
  AutoPermissionReview undecided = JudgeAnswers(argmax_only, bars);
  CHECK(undecided.decision == Decision::kAsk);
  CHECK(!undecided.error.empty());

  const Endpoint openrouter = OpenRouterEndpoint();
  ReviewBars jev = ReviewBarsFor(openrouter, "~typesafe/jev-latest");
  CHECK(jev.allow == 0.85 && jev.deny == 0.95 && jev.veto == 0.9);
  ReviewBars luna = ReviewBarsFor(openrouter, "openai/gpt-6-luna-decisions");
  CHECK(luna.allow == 0.85 && luna.veto == 0.9);
  // A model nobody calibrated is held to the strict bars.
  ReviewBars unknown = ReviewBarsFor(openrouter, "other/model");
  CHECK(unknown.allow == 0.9 && unknown.deny == 0.95 && unknown.veto == 0.3);
  Endpoint careless = openrouter;
  careless.options["decision_bars"]["careless"] = {{"allow", -1}, {"veto", 2}};
  ReviewBars kept = ReviewBarsFor(careless, "careless");
  CHECK(kept.allow == 0.9 && kept.veto == 0.3);

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

  // A free setup never silently falls back to a paid reviewer. An explicit
  // reviewer remains the user's choice.
  api.options["free_only"] = true;
  CHECK(!ResolveReviewRoute(api, {first}, "").none.empty());
  CHECK(ResolveReviewRoute(api, {first}, "first/judge-1").route.model ==
        "judge-1");

  // A path argument outside the folder is what the model never judges.
  const std::string root = CanonicalCwd();
  CHECK(!PathOutsideFolder({{"command", "ls"}}, root));
  CHECK(!PathOutsideFolder({{"path", "a.txt"}}, root));
  CHECK(PathOutsideFolder({{"path", "/etc/hosts"}}, root));
  // Where the sandbox lets commands write, a file tool is as free.
  const SandboxStatus& sandbox = SandboxRuntime();
  if (sandbox.mode == SandboxMode::kEnforced) {
    for (const std::string& writable : sandbox.policy.writable_roots) {
      CHECK(!PathOutsideFolder({{"path", writable + "/scratch.py"}}, root));
    }
  }
}

// A remote path tool's subject names where its gateway resolved the path
// (execution.cc), so a remembered rule still covers the file's folder.
TEST_CASE(TestRemotePathToolRemembersItsFolder) {
  const std::string root = "/remote/project";
  Tool tool;
  tool.name = "write_file";
  tool.approval_subject = [](const json& args) {
    return json{
        {"path", args["path"]},
        {"resolved", "/remote/project/" + args["path"].get<std::string>()}};
  };
  const PermissionRule rule =
      SuggestRule(tool, {{"path", "src/a.cc"}}, root, true);
  CHECK(rule.match == "./src/");
  CHECK(RuleCovers(rule, tool, {{"path", "src/b.cc"}}, root, true));
  CHECK(!RuleCovers(rule, tool, {{"path", "docs/b.md"}}, root, true));
  tool.approval_subject = [](const json&) { return json{{"origin", "x"}}; };
  CHECK(SuggestRule(tool, {{"path", "src/a.cc"}}, root, true).match ==
        R"({"origin":"x"})");
}

}  // namespace uagent
