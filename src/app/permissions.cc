// Copyright 2026 Timon Gentzsch

#include "include/app/permissions.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>

#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/core/private_store.h"
#include "include/core/runtime_config.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
namespace {

constexpr int64_t kPermissionStoreVersion = 1;
constexpr size_t kPermissionStoreBytes = MiB(1);
constexpr size_t kStoredPreviewBytes = 4096;
constexpr size_t kReviewerPreviewBytes = 16384;
constexpr int64_t kReviewerTimeoutSeconds = 15;
constexpr int kReviewerAttempts = 3;
constexpr char kReviewerRoute[] = "permission_review";
// The bars for a decision model no endpoint row calibrates: strict, so that
// an unknown model asks rather than allows.
constexpr ReviewBars kStrictBars{0.9, 0.95, 0.3};
// A reason below this, and below the veto, is not worth a line on the card.
constexpr double kReasonShown = 0.5;

// The reason questions of the review: each can veto an allow, and a likely
// one is shown on the approval card by its label.
struct Reason {
  const char* id;
  const char* label;
};
constexpr Reason kReasons[] = {
    {"destructive", "irreversible or destructive"},
    {"beyond_request", "beyond what you asked"},
    {"sends_data_off_machine", "sends data off this machine"},
    {"touches_secrets", "touches credentials or secrets"},
};

json EmptyStore() {
  return {{"version", kPermissionStoreVersion}, {"rules", json::array()}};
}

bool ValidStore(const json& store, std::string& error) {
  if (JsonValue(store, "version", int64_t{0}) != kPermissionStoreVersion ||
      !JsonArray(store, "rules")) {
    error = "permission rules file is invalid";
    return false;
  }
  for (const auto& rule : store["rules"]) {
    if (!JsonStringRef(rule, "root") || !JsonStringRef(rule, "key") ||
        !JsonStringRef(rule, "tool") || !JsonStringRef(rule, "preview") ||
        JsonValue(rule, "effect", "") != "allow") {
      error = "permission rules file contains an invalid rule";
      return false;
    }
  }
  return true;
}

// The rules file, locked for its lifetime. Usable() is false, with `error`
// set, when it cannot be read or fails validation.
class RulesFile : public PrivateJsonStore {
 public:
  explicit RulesFile(std::string& error)
      : PrivateJsonStore(kPermissionStoreFile, EmptyStore(),
                         kPermissionStoreBytes, error),
        usable_(Ready() && ValidStore(Data(), error)) {}

  bool Usable() const { return usable_; }
  json& Rules() { return Data()["rules"]; }

 private:
  bool usable_;
};

// Matches the rules of `root`, narrowed to `key` when one is given. Every
// stored rule allows; ValidStore rejects any other effect.
auto RuleOf(const std::string& root, const std::string* key = nullptr) {
  return [&root, key](const json& rule) {
    return JsonValue(rule, "root", "") == root &&
           (!key || JsonValue(rule, "key", "") == *key);
  };
}

std::string Percent(double probability) {
  return std::to_string(std::lround(probability * 100)) + "%";
}

bool IsProbability(double value) { return value >= 0 && value <= 1; }

// A bar from its setting; text that is no probability leaves the bar alone.
void Override(double& bar, const std::string& text) {
  double value = 0;
  if (ParseFiniteDouble(text.c_str(), value) && IsProbability(value)) {
    bar = value;
  }
}

// The number under `key` when it is a probability.
bool Probability(const json& object, const char* key, double& value) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_number()) return false;
  const double number = found->get<double>();
  if (!IsProbability(number)) return false;
  value = number;
  return true;
}

// The reviewer's refusal in words, from the status and the error envelope
// ({error: {code, message}}). Rate limits and outages were retried already.
std::string ReviewError(const JsonResponse& response) {
  switch (response.http_status) {
    case 401:
    case 403:
      return "the reviewer's API key was refused";
    case 402:
      return "the reviewer's account has no credit";
    case 413:
      return "the action is too large for the reviewer";
    case 429:
      return "the reviewer is rate limited";
    default:
      break;
  }
  const json* envelope = JsonObject(response.body, "error");
  const std::string message =
      envelope ? JsonValue(*envelope, "message", "") : std::string();
  return response.error +
         (message.empty() ? "" : ": " + Utf8Prefix(message, 200));
}

}  // namespace

std::string PermissionKey(const Tool& tool, const json& arguments,
                          ApprovalClass required) {
  json policy = {{"name", tool.name},
                 {"provider", tool.provider},
                 {"parameters", ToolParameters(tool)},
                 {"mutating", tool.mutating},
                 {"capabilities", tool.capabilities},
                 {"approval_class", static_cast<int>(required)}};
  if (!tool.output_schema.is_null()) {
    policy["output_schema"] = tool.output_schema;
  }
  return HashHex(JsonDump(
      {{"policy", std::move(policy)},
       {"arguments", tool.approval_subject ? tool.approval_subject(arguments)
                                           : arguments}}));
}

bool PathOutsideFolder(const json& arguments, const std::string& root) {
  const std::string path = JsonValue(arguments, "path", "");
  return !path.empty() && !PathWithin(CanonicalAccessPath(path), root);
}

json ApprovalRisks(const Tool& tool, const json& arguments,
                   const std::string& root) {
  auto has = [&](ToolCapability capability) {
    return (tool.capabilities & Capability(capability)) != 0;
  };
  const std::pair<bool, std::pair<const char*, const char*>> candidates[] = {
      {has(ToolCapability::kExecute), {"runs", "runs commands"}},
      {!has(ToolCapability::kExecute) && ToolMutates(tool, arguments),
       {"writes", "makes changes"}},
      {has(ToolCapability::kExternal), {"network", "uses the network"}},
      {PathOutsideFolder(arguments, root), {"outside", "outside this folder"}},
  };
  json risks = json::array();
  for (const auto& [applies, risk] : candidates) {
    if (applies) risks.push_back({{"id", risk.first}, {"label", risk.second}});
  }
  return risks;
}

bool RepositoryPermissionAllows(const std::string& root,
                                const std::string& key) {
  std::string error;
  RulesFile file(error);
  if (!file.Usable()) {
    DebugLog("permission_rules_error", {{"error", error}});
    return false;
  }
  return std::any_of(file.Rules().begin(), file.Rules().end(),
                     RuleOf(root, &key));
}

bool RememberRepositoryPermission(const std::string& root,
                                  const std::string& key,
                                  const std::string& tool,
                                  const std::string& preview,
                                  std::string& error) {
  RulesFile file(error);
  if (!file.Usable()) return false;
  json& rules = file.Rules();
  auto found = std::find_if(rules.begin(), rules.end(), RuleOf(root, &key));
  json rule = {{"root", root},
               {"key", key},
               {"effect", "allow"},
               {"tool", tool},
               {"preview", Utf8Prefix(preview, kStoredPreviewBytes)},
               {"created", NowSeconds()}};
  if (found == rules.end()) {
    rules.push_back(std::move(rule));
  } else {
    *found = std::move(rule);
  }
  return file.Save(error);
}

json PermissionRulesControl(const json& request) {
  const std::string root = CanonicalCwd();
  const std::string action = JsonValue(request, "action", "list");
  std::string error;
  RulesFile file(error);
  if (!file.Usable()) return {{"error", error}};
  json& rules = file.Rules();
  if (action == "delete" || action == "clear") {
    const std::string key = JsonValue(request, "key", "");
    const size_t before = rules.size();
    rules.erase(
        std::remove_if(rules.begin(), rules.end(),
                       RuleOf(root, action == "delete" ? &key : nullptr)),
        rules.end());
    if (action == "delete" && rules.size() == before) {
      return {{"error", "permission rule not found"}};
    }
    if (!file.Save(error)) return {{"error", error}};
  } else if (action != "list") {
    return {{"error", "unknown permission rule action"}};
  }
  json listed = json::array();
  std::copy_if(rules.begin(), rules.end(), std::back_inserter(listed),
               RuleOf(root));
  return {{"root", root}, {"rules", std::move(listed)}};
}

const json& ReviewQuestions() {
  static const json kQuestions = json::parse(R"json({
    "permission": {
      "type": "choice",
      "instructions": "Choose how this tool call should be handled. Protect the user's machine and data while allowing routine work that is clearly authorized by the user's request.",
      "criteria": {
        "allow": "The action is a reversible or routine consequence of the user's request and does not need a separate confirmation.",
        "ask": "The user's intent is ambiguous, the supplied action is incomplete, or the user should make this consequential choice.",
        "deny": "The action conflicts with the request, exceeds its scope, or creates an unacceptable security or data-loss risk."
      }
    },
    "destructive": {
      "type": "noul",
      "instructions": "Does this action irreversibly delete, overwrite or destroy data or state that cannot easily be recovered?",
      "criteria": {"true": "Yes.", "false": "No."}
    },
    "beyond_request": {
      "type": "noul",
      "instructions": "Does this action go beyond what the user's last message asked for?",
      "criteria": {"true": "Yes.", "false": "No."}
    },
    "sends_data_off_machine": {
      "type": "noul",
      "instructions": "Does this action send files, data or secrets from this machine to another host or service?",
      "criteria": {"true": "Yes.", "false": "No."}
    },
    "touches_secrets": {
      "type": "noul",
      "instructions": "Does this action read, copy or expose credentials, keys, tokens or secret files?",
      "criteria": {"true": "Yes.", "false": "No."}
    }
  })json");
  return kQuestions;
}

ReviewBars ReviewBarsFor(const Endpoint& endpoint, const std::string& model,
                         const RuntimeConfig& config) {
  ReviewBars bars = kStrictBars;
  const json* table = JsonObject(endpoint.options, "decision_bars");
  if (const json* row = table ? JsonObject(*table, model.c_str()) : nullptr) {
    // A row that states no probability keeps the strict bar.
    Probability(*row, "allow", bars.allow);
    Probability(*row, "deny", bars.deny);
    Probability(*row, "veto", bars.veto);
  }
  Override(bars.allow, config.permission_allow_above);
  Override(bars.deny, config.permission_deny_above);
  Override(bars.veto, config.permission_veto_above);
  return bars;
}

AutoPermissionReview JudgeAnswers(const json& answers, const ReviewBars& bars) {
  AutoPermissionReview review;
  const json* permission = JsonObject(answers, "permission");
  const json* probabilities =
      permission ? JsonObject(*permission, "probabilities") : nullptr;
  if (!probabilities) {
    review.error = "permission reviewer returned no decision";
    return review;
  }
  review.probabilities = *probabilities;
  double allow = 0;
  double deny = 0;
  if (!Probability(*probabilities, "allow", allow) ||
      !Probability(*probabilities, "deny", deny)) {
    review.error = "permission reviewer returned no probabilities";
    return review;
  }
  const Reason* vetoing = nullptr;
  double vetoing_probability = 0;
  for (const Reason& reason : kReasons) {
    const json* answer = JsonObject(answers, reason.id);
    double probability = 0;
    if (!answer || !Probability(*answer, "noul", probability)) {
      review.error =
          std::string("permission reviewer did not answer ") + reason.id;
      return review;
    }
    if (probability >= std::min(bars.veto, kReasonShown)) {
      review.reasons.push_back(
          {{"id", reason.id},
           {"label", std::string(reason.label) + " " + Percent(probability)}});
    }
    if (probability >= bars.veto &&
        (!vetoing || probability > vetoing_probability)) {
      vetoing = &reason;
      vetoing_probability = probability;
    }
  }
  if (deny >= bars.deny) {
    review.decision = AutoPermissionDecision::kDeny;
    review.note = "deny " + Percent(deny) + " reaches the " +
                  Percent(bars.deny) + " that refuses";
  } else if (vetoing) {
    review.note = std::string(vetoing->label) + " " +
                  Percent(vetoing_probability) + " reaches the " +
                  Percent(bars.veto) + " that asks";
  } else if (allow < bars.allow) {
    review.note = "allow " + Percent(allow) + " is below the " +
                  Percent(bars.allow) + " needed";
  } else {
    review.decision = AutoPermissionDecision::kAllow;
    review.note = "allow " + Percent(allow) + " reaches the " +
                  Percent(bars.allow) + " needed";
  }
  return review;
}

AutoPermissionReview ReviewPermission(Api& api, const RouteResolution& route,
                                      const RuntimeConfig& config,
                                      UsageAccumulator& usage,
                                      const ReviewedCall& call) {
  AutoPermissionReview result;
  if (!route.none.empty()) {
    result.error = route.none;
    return result;
  }
  const Endpoint& endpoint = route.route;
  // The address names the whole resource; the client wants it as a base and
  // a path.
  const std::string url = endpoint.Option("decisions_url");
  const size_t scheme = url.find("://");
  const size_t path = url.rfind('/');
  if (scheme == std::string::npos || path <= scheme + 2) {
    result.error = "decisions_url is not an address with a path";
    return result;
  }
  api.base_url = url.substr(0, path);
  api.api_key = endpoint.api_key;
  api.wire_api = WireApi::kChatCompletions;
  const std::string& model = route.route.model;
  json request = {
      {"model", model},
      {"state",
       {{"tool", call.tool.name},
        {"provider", call.tool.provider},
        {"working_directory", CanonicalCwd()},
        {"user_request", Utf8Prefix(call.user_request, kReviewerPreviewBytes)},
        {"action", Utf8Prefix(call.preview, kReviewerPreviewBytes)},
        {"action_truncated", call.preview.size() > kReviewerPreviewBytes}}},
      {"questions", ReviewQuestions()}};
  // Commands and paths are sent: the endpoint is asked to keep them to
  // providers that retain nothing, and sees a hash of the session, not its id.
  if (endpoint.features.Has(Feature::kProviderRouting) &&
      JsonValue(endpoint.options, "decisions_private", true)) {
    request["provider"] = {{"zdr", true}, {"data_collection", "deny"}};
  }
  if (endpoint.features.Has(Feature::kSessionPassthrough) &&
      !call.session_id.empty()) {
    request["session_id"] = TruncatedHash(call.session_id, 16);
  }
  JsonResponse response = api.Post(url.substr(path), request,
                                   kReviewerTimeoutSeconds, kReviewerAttempts);
  if (!response.error.empty()) {
    result.error = ReviewError(response);
    return result;
  }
  Usage spent;
  if (const json* value = JsonObject(response.body, "usage")) {
    spent.Add(*value);
  }
  if (HasUsage(spent)) {
    usage.Add(RouteKey(api.base_url, kReviewerRoute, model, ""), spent,
              call.turn, {{"permission_reviews", 1}});
  }
  const json* answers = JsonObject(response.body, "answers");
  return JudgeAnswers(answers ? *answers : json::object(),
                      ReviewBarsFor(endpoint, model, config));
}

}  // namespace uagent
