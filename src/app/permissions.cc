// Copyright 2026 Timon Gentzsch

#include "include/app/permissions.h"

#include <algorithm>
#include <cctype>
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
#include "include/core/workspace.h"

namespace uagent {
namespace {

constexpr int64_t kPermissionStoreVersion = 3;
constexpr const char* kRuleEffects[] = {"allow", "ask", "deny"};
constexpr size_t kPermissionStoreBytes = MiB(1);
constexpr size_t kMatchBytes = 4096;
json EmptyStore() {
  return {{"version", kPermissionStoreVersion}, {"rules", json::array()}};
}

bool ValidStore(const json& store, std::string& error) {
  if (!JsonArray(store, "rules")) {
    error = "permission rules file is invalid";
    return false;
  }
  for (const auto& rule : store["rules"]) {
    RuleEffect effect = RuleEffect::kAllow;
    if (!JsonStringRef(rule, "root") || !JsonStringRef(rule, "tool") ||
        !JsonStringRef(rule, "match") ||
        !ParseRuleEffect(JsonValue(rule, "effect", ""), effect)) {
      error = "permission rules file contains an invalid rule";
      return false;
    }
  }
  return true;
}

// The rules file, locked for its lifetime. Usable() is false, with `error`
// set, when it cannot be read, is of another format or fails validation. A
// file of another format is refused, not read as empty: saving over it would
// drop its rules.
class RulesFile : public PrivateJsonStore {
 public:
  explicit RulesFile(std::string& error)
      : PrivateJsonStore(kPermissionStoreFile, EmptyStore(),
                         kPermissionStoreBytes, error),
        usable_(Ready() && Current(error) && ValidStore(Data(), error)) {}

  bool Usable() const { return usable_; }
  json& Rules() { return Data()["rules"]; }

 private:
  bool Current(std::string& error) {
    const int64_t version = JsonValue(Data(), "version", int64_t{0});
    if (version == kPermissionStoreVersion) return true;
    error = "~/.uagent/config/" + std::string(kPermissionStoreFile) +
            " has format " + std::to_string(version) + "; this build reads " +
            std::to_string(kPermissionStoreVersion) +
            ". Remove it and remember the rules again";
    return false;
  }
  bool usable_;
};

PermissionRule RuleFrom(const json& held) {
  PermissionRule rule{JsonValue(held, "tool", ""),
                      JsonValue(held, "match", "")};
  ParseRuleEffect(JsonValue(held, "effect", ""), rule.effect);
  return rule;
}

// Matches the rules of `root`, narrowed to `rule` when one is given.
auto RuleOf(const std::string& root, const PermissionRule* rule = nullptr) {
  return [&root, rule](const json& held) {
    return JsonValue(held, "root", "") == root &&
           (!rule || (JsonValue(held, "tool", "") == rule->tool &&
                      JsonValue(held, "match", "") == rule->match));
  };
}

// The parts of a command between the operators that run one after another.
std::vector<std::string> CommandParts(const std::string& command) {
  std::vector<std::string> parts;
  std::string part;
  const auto keep = [&] {
    if (std::string trimmed = Trim(part); !trimmed.empty()) {
      parts.push_back(std::move(trimmed));
    }
    part.clear();
  };
  for (const char c : command) {
    if (c == ';' || c == '&' || c == '|' || c == '\n') {
      keep();
    } else {
      part += c;
    }
  }
  keep();
  return parts;
}

// One command with nothing that chains, substitutes or redirects: what a
// command prefix may allow.
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

}  // namespace

std::string RuleSubject(const Tool& tool, const json& arguments,
                        const std::string& root) {
  if (tool.name == "run") return Trim(JsonValue(arguments, "command", ""));
  if (tool.name == "web_fetch") return UrlHost(JsonValue(arguments, "url", ""));
  const json subject =
      tool.approval_subject ? tool.approval_subject(arguments) : json();
  // A remote path tool's subject names where its gateway resolved the path.
  const std::string* resolved = JsonStringRef(subject, "resolved");
  if (!subject.is_null() && !resolved) return JsonDump(subject);
  if (const std::string* path = JsonStringRef(arguments, "path")) {
    // Inside the folder by its place there, so a rule reads as ./src/.
    const std::filesystem::path canonical =
        resolved ? std::filesystem::path(*resolved)
                 : CanonicalAccessPath(*path);
    if (PathWithin(canonical, root)) {
      return "./" + canonical.lexically_relative(root).generic_string();
    }
    return canonical.generic_string();
  }
  return JsonDump(arguments);
}

PermissionRule SuggestRule(const Tool& tool, const json& arguments,
                           const std::string& root, bool confined) {
  const std::string subject = RuleSubject(tool, arguments, root);
  PermissionRule rule{tool.name, Utf8Prefix(subject, kMatchBytes)};
  if (tool.provider.starts_with("mcp:")) {
    rule.match = "*";
  } else if (tool.name == "run") {
    // The program and its subcommand: `npm test -- auth` is `npm test *`.
    const std::vector<std::string> words = Words(subject);
    if (confined && SimpleCommand(subject) && !words.empty() &&
        PlainWord(words[0])) {
      std::string prefix = words[0];
      if (words.size() > 1 && PlainWord(words[1])) prefix += " " + words[1];
      // A lone program takes any flags; a program and its subcommand any
      // arguments. A plain word after flags is a subcommand left exact.
      const bool exact = words.size() > 2 && prefix == words[0];
      if (!exact) rule.match = prefix + " *";
    }
  } else if (subject.starts_with("./") || subject.starts_with("/")) {
    // A path: the file's folder.
    const size_t slash = subject.rfind('/');
    if (slash != std::string::npos) rule.match = subject.substr(0, slash + 1);
  }
  return rule;
}

bool RuleCovers(const PermissionRule& rule, const Tool& tool,
                const json& arguments, const std::string& root, bool confined) {
  if (rule.tool != tool.name) return false;
  if (rule.match == "*") return true;
  const std::string subject = RuleSubject(tool, arguments, root);
  if (rule.match.ends_with("/")) return subject.starts_with(rule.match);
  const bool prefixed = rule.match.ends_with(" *");
  const std::string prefix =
      prefixed ? rule.match.substr(0, rule.match.size() - 2) : rule.match;
  const auto matches = [&](const std::string& part) {
    return part == prefix || (prefixed && part.starts_with(prefix + " "));
  };
  // A rule that holds a call back holds every form of it, a part of a
  // longer command too; one that lets it through, only the whole of a
  // simple command the sandbox holds.
  if (rule.effect != RuleEffect::kAllow) {
    return matches(subject) ||
           (tool.name == "run" &&
            std::ranges::any_of(CommandParts(subject), matches));
  }
  if (!prefixed) return subject == rule.match;
  return confined && SimpleCommand(subject) && matches(subject);
}

const char* RuleEffectName(RuleEffect effect) {
  return kRuleEffects[static_cast<size_t>(effect)];
}

bool ParseRuleEffect(std::string_view name, RuleEffect& effect) {
  const auto found = std::ranges::find(kRuleEffects, name);
  if (found == std::end(kRuleEffects)) return false;
  effect = static_cast<RuleEffect>(found - std::begin(kRuleEffects));
  return true;
}

std::string RuleLabel(const PermissionRule& rule) {
  return rule.match == "*" ? "every " + rule.tool + " call"
                           : rule.tool + " " + Utf8Prefix(rule.match, 120);
}

bool PathOutsideFolder(const json& arguments, const std::string& root) {
  const std::string path = JsonValue(arguments, "path", "");
  if (RemoteWorkspace() && !path.empty()) {
    const json checked = WorkspaceRequest(
        {{"op", "check"}, {"tool", "read_path"}, {"args", {{"path", path}}}});
    return JsonValue(checked.value("scope", json::object()), "outside", true);
  }
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

void SessionRules::Add(PermissionRule rule) {
  std::lock_guard lock(mutex_);
  rules_.push_back(std::move(rule));
}

void SessionRules::Clear() {
  std::lock_guard lock(mutex_);
  rules_.clear();
}

std::optional<PermissionRule> SessionRules::Covering(const Tool& tool,
                                                     const json& arguments,
                                                     const std::string& root,
                                                     bool confined) const {
  std::lock_guard lock(mutex_);
  for (const PermissionRule& rule : rules_) {
    if (RuleCovers(rule, tool, arguments, root, confined)) return rule;
  }
  return std::nullopt;
}

CoveringRules FolderRules(const std::string& root, const Tool& tool,
                          const json& arguments, bool confined) {
  CoveringRules covering;
  RulesFile file(covering.error);
  if (!file.Usable()) {
    DebugLog("permission_rules_error", {{"error", covering.error}});
    return covering;
  }
  const std::string identity = WorkspaceIdentity{Workspace().host, root}.Key();
  for (const json& held : file.Rules()) {
    PermissionRule rule = RuleFrom(held);
    if (JsonValue(held, "root", "") != identity ||
        !RuleCovers(rule, tool, arguments, root, confined)) {
      continue;
    }
    std::optional<PermissionRule>& slot =
        rule.effect == RuleEffect::kDeny  ? covering.deny
        : rule.effect == RuleEffect::kAsk ? covering.ask
                                          : covering.allow;
    if (!slot) slot = std::move(rule);
  }
  return covering;
}

namespace {
// Keeps `rule` for `identity` in place of one with its tool and match.
void Keep(json& rules, const std::string& identity,
          const PermissionRule& rule) {
  rules.erase(
      std::remove_if(rules.begin(), rules.end(), RuleOf(identity, &rule)),
      rules.end());
  rules.push_back({{"root", identity},
                   {"tool", rule.tool},
                   {"match", rule.match},
                   {"effect", RuleEffectName(rule.effect)},
                   {"created", NowSeconds()}});
}
}  // namespace

bool RememberRepositoryRule(const std::string& root, const PermissionRule& rule,
                            std::string& error) {
  RulesFile file(error);
  if (!file.Usable()) return false;
  Keep(file.Rules(), WorkspaceIdentity{Workspace().host, root}.Key(), rule);
  return file.Save(error);
}

json PermissionRulesControl(const json& request) {
  const std::string root = Workspace().Key();
  const std::string action = JsonValue(request, "action", "list");
  std::string error;
  RulesFile file(error);
  if (!file.Usable()) return {{"error", error}};
  json& rules = file.Rules();
  PermissionRule rule{JsonValue(request, "tool", ""),
                      JsonValue(request, "match", "")};
  if (action == "add") {
    if (rule.tool.empty() || rule.match.empty() ||
        !ParseRuleEffect(JsonValue(request, "effect", ""), rule.effect)) {
      return {{"error",
               "a rule needs a tool, a match and an effect: allow, "
               "ask or deny"}};
    }
    Keep(rules, root, rule);
    if (!file.Save(error)) return {{"error", error}};
  } else if (action == "delete" || action == "clear") {
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

}  // namespace uagent
