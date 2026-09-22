// Copyright 2026 Timon Gentzsch

#include "include/app/permissions.h"

#include <algorithm>
#include <string>
#include <utility>

#include "include/app/private_store.h"
#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
namespace {

constexpr int64_t kPermissionStoreVersion = 1;
constexpr size_t kPermissionStoreBytes = size_t{1024} * 1024;
constexpr size_t kStoredPreviewBytes = 4096;
constexpr size_t kReviewerPreviewBytes = 16384;
constexpr int64_t kReviewerTimeoutSeconds = 15;
constexpr int kReviewerAttempts = 3;
constexpr char kPermissionStoreFile[] = "permissions.json";
constexpr char kReviewerRoute[] = "permission_review";

json EmptyStore() {
  return {{"version", kPermissionStoreVersion}, {"rules", json::array()}};
}

bool ValidStore(const json& store, std::string& error) {
  if (!store.is_object() ||
      JsonValue(store, "version", int64_t{0}) != kPermissionStoreVersion ||
      !store.contains("rules") || !store["rules"].is_array()) {
    error = "permission rules file is invalid";
    return false;
  }
  for (const auto& rule : store["rules"]) {
    if (!rule.is_object() || !rule.contains("root") ||
        !rule["root"].is_string() || !rule.contains("key") ||
        !rule["key"].is_string() || !rule.contains("tool") ||
        !rule["tool"].is_string() || !rule.contains("preview") ||
        !rule["preview"].is_string() ||
        JsonValue(rule, "effect", "") != "allow") {
      error = "permission rules file contains an invalid rule";
      return false;
    }
  }
  return true;
}

std::string PrefixBytes(const std::string& value, size_t limit,
                        bool* truncated = nullptr) {
  if (truncated) *truncated = value.size() > limit;
  if (value.size() <= limit) return value;
  return value.substr(0, limit);
}

Usage DecisionUsage(const json& response) {
  const json* value = JsonObject(response, "usage");
  if (!value) {
    const json* metadata = JsonObject(response, "provider_metadata");
    const json* openrouter =
        metadata ? JsonObject(*metadata, "openrouter") : nullptr;
    value = openrouter ? JsonObject(*openrouter, "usage") : nullptr;
  }
  Usage parsed;
  if (value) parsed.Add(*value);
  return parsed;
}

}  // namespace

const char* PermissionOverrideName(PermissionOverride mode) {
  switch (mode) {
    case PermissionOverride::kDefault:
      return "default";
    case PermissionOverride::kAsk:
      return "ask";
    case PermissionOverride::kAuto:
      return "auto";
    case PermissionOverride::kYolo:
      return "yolo";
  }
  return "default";
}

bool ParsePermissionOverride(const std::string& value,
                             PermissionOverride& mode) {
  if (value == "default") {
    mode = PermissionOverride::kDefault;
    return true;
  }
  ApprovalMode parsed;
  if (!ParseApprovalMode(value, parsed)) return false;
  mode = parsed == ApprovalMode::kYolo   ? PermissionOverride::kYolo
         : parsed == ApprovalMode::kAuto ? PermissionOverride::kAuto
                                         : PermissionOverride::kAsk;
  return true;
}

PermissionOverride LegacyPermissionOverride(const json& value) {
  if (value.is_string()) {
    PermissionOverride parsed;
    if (ParsePermissionOverride(value.get<std::string>(), parsed))
      return parsed;
  }
  if (value.is_number_integer()) {
    const int legacy = value.get<int>();
    if (legacy == 0) return PermissionOverride::kAsk;
    if (legacy == 1) return PermissionOverride::kYolo;
  }
  return PermissionOverride::kDefault;
}

std::string PermissionKey(const Tool& tool, const json& arguments,
                          ApprovalClass required) {
  json policy = {{"name", tool.name},
                 {"provider", tool.provider},
                 {"parameters", ToolParameters(tool)},
                 {"mutating", tool.mutating},
                 {"capabilities", tool.capabilities},
                 {"approval_class", static_cast<int>(required)}};
  if (!tool.output_schema.is_null())
    policy["output_schema"] = tool.output_schema;
  return HashHex(
      JsonDump({{"policy", std::move(policy)}, {"arguments", arguments}}));
}

const char* PermissionScope() { return "this exact action"; }

bool RepositoryPermissionAllows(const std::string& root,
                                const std::string& key) {
  std::string error;
  PrivateJsonStore file(kPermissionStoreFile, EmptyStore(),
                        kPermissionStoreBytes, error);
  if (!file.Ready() || !ValidStore(file.Data(), error)) {
    DebugLog("permission_rules_error", {{"error", error}});
    return false;
  }
  return std::any_of(file.Data()["rules"].begin(), file.Data()["rules"].end(),
                     [&](const json& rule) {
                       return JsonValue(rule, "root", "") == root &&
                              JsonValue(rule, "key", "") == key &&
                              JsonValue(rule, "effect", "") == "allow";
                     });
}

bool RememberRepositoryPermission(const std::string& root,
                                  const std::string& key,
                                  const std::string& tool,
                                  const std::string& preview,
                                  std::string& error) {
  PrivateJsonStore file(kPermissionStoreFile, EmptyStore(),
                        kPermissionStoreBytes, error);
  if (!file.Ready() || !ValidStore(file.Data(), error)) return false;
  json& store = file.Data();
  auto found = std::find_if(store["rules"].begin(), store["rules"].end(),
                            [&](const json& rule) {
                              return JsonValue(rule, "root", "") == root &&
                                     JsonValue(rule, "key", "") == key;
                            });
  json rule = {{"root", root},
               {"key", key},
               {"effect", "allow"},
               {"tool", tool},
               {"preview", PrefixBytes(preview, kStoredPreviewBytes)},
               {"created", NowSeconds()}};
  if (found == store["rules"].end()) {
    store["rules"].push_back(std::move(rule));
  } else {
    *found = std::move(rule);
  }
  return file.Save(error);
}

json PermissionRulesControl(const json& request) {
  const std::string root = CanonicalCwd();
  const std::string action = JsonValue(request, "action", "list");
  std::string error;
  PrivateJsonStore file(kPermissionStoreFile, EmptyStore(),
                        kPermissionStoreBytes, error);
  if (!file.Ready() || !ValidStore(file.Data(), error)) {
    return {{"error", error}};
  }
  json& store = file.Data();
  if (action == "delete") {
    const std::string key = JsonValue(request, "key", "");
    auto& rules = store["rules"];
    const size_t before = rules.size();
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [&](const json& rule) {
                                 return JsonValue(rule, "root", "") == root &&
                                        JsonValue(rule, "key", "") == key;
                               }),
                rules.end());
    if (rules.size() == before) return {{"error", "permission rule not found"}};
    if (!file.Save(error)) return {{"error", error}};
  } else if (action == "clear") {
    auto& rules = store["rules"];
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [&](const json& rule) {
                                 return JsonValue(rule, "root", "") == root;
                               }),
                rules.end());
    if (!file.Save(error)) return {{"error", error}};
  } else if (action != "list") {
    return {{"error", "unknown permission rule action"}};
  }
  json rules = json::array();
  for (const auto& rule : store["rules"]) {
    if (JsonValue(rule, "root", "") == root) rules.push_back(rule);
  }
  return {{"root", root}, {"rules", std::move(rules)}};
}

AutoPermissionReview ReviewPermission(Api& api, const RuntimeConfig& config,
                                      UsageAccumulator& usage, int64_t turn,
                                      const Tool& tool,
                                      const std::string& preview,
                                      const std::string& user_request) {
  AutoPermissionReview result;
  api.base_url = config.permission_url;
  api.api_key = EnvStr("OPENROUTER_API_KEY");
  api.capabilities.wire_api = WireApi::kChatCompletions;
  api.render_stream = false;
  if (api.api_key.empty()) {
    result.error = "OPENROUTER_API_KEY is not configured";
    return result;
  }
  bool truncated = false;
  std::string shown = PrefixBytes(preview, kReviewerPreviewBytes, &truncated);
  const std::string model = config.permission_model;
  json request = {
      {"model", model},
      {"state",
       {{"tool", tool.name},
        {"provider", tool.provider},
        {"working_directory", CanonicalCwd()},
        {"user_request", user_request},
        {"action", std::move(shown)},
        {"action_truncated", truncated}}},
      {"questions",
       {{"permission",
         {{"type", "choice"},
          {"instructions",
           "Choose how this tool call should be handled. Protect the user's "
           "machine and data while allowing routine work that is clearly "
           "authorized by the user's request."},
          {"criteria",
           {{"allow",
             "The action is a reversible or routine consequence of the "
             "user's request and does not need a separate confirmation."},
            {"ask",
             "The user's intent is ambiguous, the supplied action is "
             "incomplete, or the user should make this consequential choice."},
            {"deny",
             "The action conflicts with the request, exceeds its scope, or "
             "creates an unacceptable security or data-loss risk."}}}}}}}};
  JsonResponse response = api.Post("/decisions", request,
                                   kReviewerTimeoutSeconds, kReviewerAttempts);
  if (!response.error.empty()) {
    result.error = response.error;
    return result;
  }
  const Usage spent = DecisionUsage(response.body);
  if (HasUsage(spent)) {
    usage.Add(RouteKey(api.base_url, kReviewerRoute, model, ""), spent, turn,
              {{"permission_reviews", 1}});
  }
  const json* answers = JsonObject(response.body, "answers");
  const json* answer = answers ? JsonObject(*answers, "permission") : nullptr;
  if (!answer) {
    result.error = "permission reviewer returned no decision";
    return result;
  }
  result.probabilities = JsonValue(*answer, "probabilities", json::object());
  const std::string choice = JsonValue(*answer, "choice", "");
  if (choice == "allow") {
    result.decision = AutoPermissionDecision::kAllow;
  } else if (choice == "deny") {
    result.decision = AutoPermissionDecision::kDeny;
  } else if (choice != "ask") {
    result.error = "permission reviewer returned an unknown decision";
  }
  return result;
}

}  // namespace uagent
