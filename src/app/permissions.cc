// Copyright 2026 Timon Gentzsch

#include "include/app/permissions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/core/private_store.h"
#include "include/core/runtime_config.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
namespace {

constexpr int64_t kPermissionStoreVersion = 2;
constexpr size_t kPermissionStoreBytes = MiB(1);
constexpr size_t kMatchBytes = 4096;
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
  if (!JsonArray(store, "rules")) {
    error = "permission rules file is invalid";
    return false;
  }
  for (const auto& rule : store["rules"]) {
    if (!JsonStringRef(rule, "root") || !JsonStringRef(rule, "tool") ||
        !JsonStringRef(rule, "match")) {
      error = "permission rules file contains an invalid rule";
      return false;
    }
  }
  return true;
}

// The rules file, locked for its lifetime. Usable() is false, with `error`
// set, when it cannot be read or fails validation. Rules of an earlier
// version, remembered as exact calls, are dropped.
class RulesFile : public PrivateJsonStore {
 public:
  explicit RulesFile(std::string& error)
      : PrivateJsonStore(kPermissionStoreFile, EmptyStore(),
                         kPermissionStoreBytes, error),
        usable_(Ready() && (Current() || ValidStore(Data(), error))) {}

  bool Usable() const { return usable_; }
  json& Rules() { return Data()["rules"]; }

 private:
  // False for a current file, which is then validated.
  bool Current() {
    if (JsonValue(Data(), "version", int64_t{0}) == kPermissionStoreVersion) {
      return false;
    }
    Data() = EmptyStore();
    return true;
  }
  bool usable_;
};

// Matches the rules of `root`, narrowed to `rule` when one is given.
auto RuleOf(const std::string& root, const PermissionRule* rule = nullptr) {
  return [&root, rule](const json& held) {
    return JsonValue(held, "root", "") == root &&
           (!rule || (JsonValue(held, "tool", "") == rule->tool &&
                      JsonValue(held, "match", "") == rule->match));
  };
}

// One command with nothing that chains, substitutes or redirects: what a
// command prefix may cover.
bool SimpleCommand(const std::string& command) {
  return !command.empty() &&
         command.find_first_of(";&|`$<>()\n\\") == std::string::npos;
}

std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> words;
  std::istringstream stream(text);
  for (std::string word; stream >> word;) words.push_back(std::move(word));
  return words;
}

// A word a prefix may stand on: a program or subcommand, not a flag, path or
// assignment.
bool PlainWord(const std::string& word) {
  return !word.empty() && word[0] != '-' &&
         std::ranges::all_of(word, [](unsigned char c) {
           return std::isalnum(c) || c == '_' || c == '-' || c == '.' ||
                  c == ':' || c == '@' || c == '+';
         });
}

// Programs that run whatever their arguments say: only their exact call is
// remembered.
bool RunsAnything(const std::string& program) {
  static constexpr std::string_view kPrograms[] = {
      "sh",   "bash",   "zsh",     "dash", "fish",  "env",      "sudo",
      "doas", "su",     "eval",    "exec", "xargs", "find",     "rm",
      "dd",   "python", "python3", "node", "perl",  "ruby",     "php",
      "npx",  "bunx",   "uvx",     "deno", "bun",   "osascript"};
  return std::ranges::find(kPrograms, program) != std::end(kPrograms);
}

std::string Percent(double probability) {
  return std::to_string(std::lround(probability * 100)) + "%";
}

bool IsProbability(double value) { return value >= 0 && value <= 1; }

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

std::string RuleSubject(const Tool& tool, const json& arguments,
                        const std::string& root) {
  if (tool.name == "run") return Trim(JsonValue(arguments, "command", ""));
  if (tool.name == "web_fetch") return UrlHost(JsonValue(arguments, "url", ""));
  if (tool.approval_subject) return JsonDump(tool.approval_subject(arguments));
  if (const std::string* path = JsonStringRef(arguments, "path")) {
    // Inside the folder by its place there, so a rule reads as ./src/.
    const std::filesystem::path canonical = CanonicalAccessPath(*path);
    if (PathWithin(canonical, root)) {
      return "./" + canonical.lexically_relative(root).generic_string();
    }
    return canonical.generic_string();
  }
  return JsonDump(arguments);
}

PermissionRule SuggestRule(const Tool& tool, const json& arguments,
                           const std::string& root) {
  const std::string subject = RuleSubject(tool, arguments, root);
  PermissionRule rule{tool.name, Utf8Prefix(subject, kMatchBytes)};
  if (tool.provider.starts_with("mcp:")) {
    rule.match = "*";
  } else if (tool.name == "run") {
    // The program and its subcommand: `npm test -- auth` is `npm test *`.
    const std::vector<std::string> words = Words(subject);
    if (SimpleCommand(subject) && !words.empty() && PlainWord(words[0]) &&
        !RunsAnything(words[0])) {
      std::string prefix = words[0];
      if (words.size() > 1 && PlainWord(words[1])) prefix += " " + words[1];
      // A lone program takes any flags; a program and its subcommand any
      // arguments. A plain word after flags is a subcommand left exact.
      const bool exact = words.size() > 2 && prefix == words[0];
      if (!exact) rule.match = prefix + " *";
    }
  } else if (!tool.approval_subject && JsonStringRef(arguments, "path")) {
    // The file's folder.
    const size_t slash = subject.rfind('/');
    if (slash != std::string::npos) rule.match = subject.substr(0, slash + 1);
  }
  return rule;
}

bool RuleCovers(const PermissionRule& rule, const Tool& tool,
                const json& arguments, const std::string& root) {
  if (rule.tool != tool.name) return false;
  if (rule.match == "*") return true;
  const std::string subject = RuleSubject(tool, arguments, root);
  if (rule.match.ends_with(" *")) {
    const std::string prefix = rule.match.substr(0, rule.match.size() - 2);
    return SimpleCommand(subject) &&
           (subject == prefix || subject.starts_with(prefix + " "));
  }
  if (rule.match.ends_with("/")) return subject.starts_with(rule.match);
  return subject == rule.match;
}

std::string RuleLabel(const PermissionRule& rule) {
  return rule.match == "*" ? "every " + rule.tool + " call"
                           : rule.tool + " " + Utf8Prefix(rule.match, 120);
}

bool PathOutsideFolder(const json& arguments, const std::string& root) {
  const std::string path = JsonValue(arguments, "path", "");
  return !path.empty() && !WithinWorkArea(path, root);
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

bool RepositoryRuleAllows(const std::string& root, const Tool& tool,
                          const json& arguments, PermissionRule* matched) {
  std::string error;
  RulesFile file(error);
  if (!file.Usable()) {
    DebugLog("permission_rules_error", {{"error", error}});
    return false;
  }
  for (const json& held : file.Rules()) {
    PermissionRule rule{JsonValue(held, "tool", ""),
                        JsonValue(held, "match", "")};
    if (JsonValue(held, "root", "") == root &&
        RuleCovers(rule, tool, arguments, root)) {
      if (matched) *matched = std::move(rule);
      return true;
    }
  }
  return false;
}

bool RememberRepositoryRule(const std::string& root, const PermissionRule& rule,
                            std::string& error) {
  RulesFile file(error);
  if (!file.Usable()) return false;
  json& rules = file.Rules();
  if (std::none_of(rules.begin(), rules.end(), RuleOf(root, &rule))) {
    rules.push_back({{"root", root},
                     {"tool", rule.tool},
                     {"match", rule.match},
                     {"created", NowSeconds()}});
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
    const PermissionRule rule{JsonValue(request, "tool", ""),
                              JsonValue(request, "match", "")};
    const size_t before = rules.size();
    rules.erase(
        std::remove_if(rules.begin(), rules.end(),
                       RuleOf(root, action == "delete" ? &rule : nullptr)),
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

ReviewBars ReviewBarsFor(const Endpoint& endpoint, const std::string& model) {
  ReviewBars bars = kStrictBars;
  const json* table = JsonObject(endpoint.options, "decision_bars");
  if (const json* row = table ? JsonObject(*table, model.c_str()) : nullptr) {
    // A row that states no probability keeps the strict bar.
    Probability(*row, "allow", bars.allow);
    Probability(*row, "deny", bars.deny);
    Probability(*row, "veto", bars.veto);
  }
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
                      ReviewBarsFor(endpoint, model));
}

}  // namespace uagent
