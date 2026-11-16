// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "include/agent/child_agent.h"
#include "include/agent/path_policy.h"
#include "include/agent/session_role.h"
#include "include/app/approval_policy.h"
#include "include/app/approval_review.h"
#include "include/app/permissions.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/workspace.h"
#include "include/ui/presentation.h"
#include "src/app/bootstrap_internal.h"

namespace uagent {

// A y/N prompt. Silence and EOF both decline: an unattended run must never
// grant trust or approval by accident.
bool Confirm(InteractionRequest request) {
  bool cancelled = false;
  bool eof = false;
  std::string answer = ReadChoiceLine(std::move(request), cancelled, eof);
  return !cancelled && !eof &&
         (answer == "y" || answer == "Y" || answer == "yes");
}

// A mandatory-human decision needs a person: a terminal or a runtime's
// clients. Headless runs and piped input cannot supply one.
bool InteractiveApprovalAvailable() {
  return isatty(STDIN_FILENO) || InteractiveReadAvailable();
}

namespace {
// What a person may answer an ordinary approval with. An answer outside the
// table is guidance: denied, and queued as steering.
enum class ApprovalAnswer {
  kOnce,
  kSession,
  kRepository,
  kNever,
  kDeny,
  kGuidance
};

struct ApprovalChoice {
  ApprovalAnswer answer;
  std::string_view value;
  std::string_view label;
  std::array<std::string_view, 3> aliases;
};

constexpr ApprovalChoice kApprovalChoices[] = {
    {ApprovalAnswer::kOnce, kAllowOnce, "Allow once", {kAllowOnce, "yes"}},
    {ApprovalAnswer::kSession,
     kAllowSession,
     "Allow for this conversation",
     {kAllowSession, "session"}},
    {ApprovalAnswer::kRepository,
     kAllowFolder,
     "Always allow here: ",
     {kAllowFolder, "always", "repository"}},
    {ApprovalAnswer::kNever,
     kDenyFolder,
     "Never allow here: ",
     {kDenyFolder, "never"}},
    {ApprovalAnswer::kDeny, kDeny, "Deny", {kDeny, "no"}},
    {ApprovalAnswer::kGuidance, "guidance", "Send guidance", {}},
};

json ApprovalOptions(const PermissionRule& rule) {
  json options = json::array();
  for (const ApprovalChoice& choice : kApprovalChoices) {
    std::string label(choice.label);
    if (label.ends_with(": ")) label += RuleLabel(rule);
    options.push_back({{"value", choice.value}, {"label", label}});
  }
  return options;
}

ApprovalAnswer ParseApprovalAnswer(const std::string& answer) {
  const std::string lowered = AsciiLower(answer);
  if (lowered.empty()) return ApprovalAnswer::kGuidance;
  for (const ApprovalChoice& choice : kApprovalChoices) {
    if (std::ranges::find(choice.aliases, lowered) != choice.aliases.end()) {
      return choice.answer;
    }
  }
  return ApprovalAnswer::kGuidance;
}

bool WrapsCommands(const Tool& tool) { return tool.name == "run"; }

// Why a mandatory call needs a person. A tool that escalates for its own
// reason names it; otherwise its path did, by reaching µAgent's
// configuration or one of the person's folders.
std::string MandatoryReason(const Tool& tool, const json& arguments) {
  const std::string reason =
      tool.mandatory_reason ? tool.mandatory_reason(arguments) : "";
  if (!reason.empty()) return reason;
  return UnreadablePath(JsonValue(arguments, "path", std::string()))
             ? "reads your folders outside this folder; sandbox.read opens one"
             : "changes \u00b5Agent's own configuration";
}

// A refusal says what was not approved, not who refused it: a client with
// nobody behind it may have declined.
struct Verdict {
  bool granted = true;
  std::string refusal = "this action was not approved";
};

// The review of each action in the turn being run, so the same action asked
// again in that turn is not paid for again.
struct ReviewMemo {
  std::mutex mutex;
  int64_t turn = -1;
  std::unordered_map<std::string, AutoPermissionReview> reviews;
};

// Nothing when the review asks for a person to decide.
std::optional<Verdict> ReviewAutomatically(AppContext* app, ReviewMemo& memo,
                                           const Tool& tool,
                                           const std::string& raw_payload,
                                           int64_t turn, json& review) {
  Verdict verdict;
  // The latest message, then the first: an agent's brief still authorizes
  // what a later note from its parent adds.
  const Conversation& history = app->agent->History();
  std::string request = history.UserMessageText(history.UserTurns());
  if (const std::string first = history.UserMessageText(1); first != request) {
    request += "\n\n[first message]\n" + first;
  }
  const std::string memo_key = tool.name + ":" + HashHex(raw_payload);
  std::optional<AutoPermissionReview> memoized;
  {
    std::lock_guard lock(memo.mutex);
    if (memo.turn != turn) {
      memo.turn = turn;
      memo.reviews.clear();
    }
    if (auto found = memo.reviews.find(memo_key); found != memo.reviews.end()) {
      memoized = found->second;
    }
  }
  AutoPermissionReview decision =
      memoized
          ? std::move(*memoized)
          : ReviewPermission(
                app->runtime.permission_api,
                ResolveReviewRoute(app->runtime.api, app->provider.providers,
                                   app->runtime.config.permission_model),
                app->runtime.side_usage,
                {tool, raw_payload, request, app->agent->SessionId(), turn});
  if (!memoized && decision.error.empty()) {
    std::lock_guard lock(memo.mutex);
    if (memo.turn == turn) memo.reviews.emplace(memo_key, decision);
  }
  const bool ask = decision.decision == AutoPermissionDecision::kAsk;
  const bool allow = decision.decision == AutoPermissionDecision::kAllow;
  review = {{"choice", ask ? "ask" : (allow ? "allow" : "deny")},
            {"probabilities", std::move(decision.probabilities)},
            {"reasons", std::move(decision.reasons)},
            {"note", std::move(decision.note)}};
  if (!decision.error.empty()) review["error"] = decision.error;
  // An undecided review with nobody to ask is a denial.
  const bool decided = !ask || !InteractiveApprovalAvailable();
  if (decided) {
    verdict.granted = allow;
    verdict.refusal =
        ask ? "the automatic permission review could not decide "
              "this action and nobody is here to ask"
            : "the automatic permission review refused this action";
    if (!decision.error.empty()) verdict.refusal += " (" + decision.error + ")";
  }
  DebugLog("permission_review", {{"tool", tool.name}, {"result", review}});
  if (!decided) return std::nullopt;
  return verdict;
}

Verdict AskPerson(AppContext* app, const Tool& tool, const json& arguments,
                  const std::string& raw_payload, const std::string& request_id,
                  const std::string& root, const PermissionRule& rule,
                  bool mandatory, const json& review) {
  Verdict verdict;
  const std::string reason = MandatoryReason(tool, arguments);
  // The review's reasons join the risks; its note names the bar it fell on.
  json risks = ApprovalRisks(tool, arguments, root);
  for (const json& caution : JsonValue(review, "reasons", json::array())) {
    risks.push_back(caution);
  }
  std::string verdict_line = JsonValue(review, "error", "");
  verdict_line = verdict_line.empty() ? JsonValue(review, "note", "")
                                      : "could not decide: " + verdict_line;
  // The preview is the whole payload: a long command is never truncated.
  Emit(Event{
      EventId::kApprovalRequested,
      {{"id", request_id},
       {"tool", tool.name},
       {"preview", TerminalSafe(raw_payload)},
       {"risks", std::move(risks)},
       {"review", verdict_line.empty() ? "" : "Auto review " + verdict_line},
       {"mandatory_human", mandatory},
       {"mandatory_reason", mandatory ? reason : std::string()}}});
  InteractionRequest request{
      .id = request_id,
      .kind = "approval",
      .prompt = "Allow " + TerminalSafe(tool.name) + "?"};
  if (mandatory) {
    request.options = json::array({{{"value", kAllowOnce}, {"label", "Allow"}},
                                   {{"value", kDeny}, {"label", "Deny"}}});
    verdict.granted = Confirm(std::move(request));
    verdict.refusal += " (" + reason + ")";
  } else {
    request.options = ApprovalOptions(rule);
    bool cancelled = false;
    bool eof = false;
    std::string answer =
        Trim(ReadChoiceLine(std::move(request), cancelled, eof));
    const ApprovalAnswer chosen =
        cancelled || eof ? ApprovalAnswer::kDeny : ParseApprovalAnswer(answer);
    verdict.granted = chosen == ApprovalAnswer::kOnce ||
                      chosen == ApprovalAnswer::kSession ||
                      chosen == ApprovalAnswer::kRepository;
    if (chosen == ApprovalAnswer::kSession) app->session_rules.push_back(rule);
    if (chosen == ApprovalAnswer::kRepository ||
        chosen == ApprovalAnswer::kNever) {
      PermissionRule kept = rule;
      if (chosen == ApprovalAnswer::kNever) kept.effect = RuleEffect::kDeny;
      std::string error;
      if (!RememberRepositoryRule(root, kept, error)) {
        Emit(NoticeEvent(
            PresentationStatus::kWarned,
            std::string(chosen == ApprovalAnswer::kNever ? "refused once"
                                                         : "allowed once") +
                "; could not save permission rule: " + error));
      }
    }
    if (chosen == ApprovalAnswer::kGuidance && !answer.empty()) {
      SteeringState().Queue(answer);
    }
  }
  return verdict;
}

}  // namespace

// The facts of the call and the situation go to Decide (approval_policy.h);
// what it decides is carried out here: a review, a person asked, a rule kept.
Agent::Approver MakeApprover(AppContext* app) {
  auto memo = std::make_shared<ReviewMemo>();
  return [app, memo](const Tool& tool, const json& arguments, int64_t turn) {
    static std::atomic<uint64_t> sequence{0};
    const std::string root = Workspace().path;
    const SandboxStatus& sandbox = SandboxRuntime();
    const json agent = app->options.AgentRole();
    PolicyContext context{
        .mode = CurrentApprovalMode(),
        .sandbox_enforced = sandbox.mode == SandboxMode::kEnforced,
        .sandbox_read_only = sandbox.policy.read_only,
        .agent_edits =
            !agent.empty() && JsonValue(agent, "edits", "") != "none",
        .person = InteractiveApprovalAvailable()};
    ApprovalFacts facts{.required = RequiredApproval(tool, arguments),
                        .mutates = ToolMutates(tool, arguments),
                        .executes = (tool.capabilities &
                                     Capability(ToolCapability::kExecute)) != 0,
                        .command = WrapsCommands(tool)};
    const bool mandatory = facts.required == ApprovalClass::kMandatoryHuman;
    if (context.mode == ApprovalMode::kPlan) {
      constexpr uint32_t kFiles = Capability(ToolCapability::kInspect) |
                                  Capability(ToolCapability::kMutate);
      const std::string plan = PlanFile();
      facts.on_plan_file =
          !plan.empty() && tool.capabilities != 0 &&
          (tool.capabilities & ~kFiles) == 0 &&
          CanonicalAccessPath(JsonValue(arguments, "path", "")) ==
              CanonicalAccessPath(plan);
    }
    if (context.mode == ApprovalMode::kAuto) {
      facts.outside_folder = PathOutsideFolder(arguments, root);
      facts.inside_work_area = context.agent_edits && tool.needs_approval &&
                               !tool.needs_approval(arguments);
    }
    // The folder's rules hold in every mode; one that refuses holds for a
    // mandatory call too. A command's prefix lets a call through only where
    // the sandbox holds it.
    const bool confined = !facts.command || context.sandbox_enforced;
    const CoveringRules folder = FolderRules(root, tool, arguments, confined);
    context.deny_rule = folder.deny.has_value();
    context.ask_rule = folder.ask.has_value();
    std::optional<PermissionRule> allowing;
    if (!mandatory) {
      const auto held =
          std::ranges::find_if(app->session_rules, [&](const auto& kept) {
            return RuleCovers(kept, tool, arguments, root, confined);
          });
      if (held != app->session_rules.end()) {
        context.session_rule = true;
        allowing = *held;
      } else if (folder.allow) {
        context.folder_rule = true;
        allowing = folder.allow;
      }
    }
    // What a person is offered to remember.
    const PermissionRule offered = SuggestRule(tool, arguments, root, confined);
    const ApprovalDecision decision = Decide(facts, context);
    const std::optional<PermissionRule>& decisive =
        decision.source == ApprovalSource::kDenyRule  ? folder.deny
        : decision.source == ApprovalSource::kAskRule ? folder.ask
                                                      : allowing;
    bool automatic = decision.effect != ApprovalEffect::kAskPerson;
    Verdict verdict;
    if (decision.source == ApprovalSource::kDenyRule) {
      verdict = {false,
                 "this action was not approved: a rule of this folder "
                 "refuses it (" +
                     RuleLabel(*folder.deny) + ")"};
    } else if (decision.source == ApprovalSource::kPlan) {
      verdict = {false,
                 "plan mode is read-only; write the plan to " + PlanFile()};
    } else if (decision.source == ApprovalSource::kNobody) {
      verdict = {false, mandatory ? "this action needs a person's approval (" +
                                        MandatoryReason(tool, arguments) +
                                        ") and nobody is here to give it"
                                  : "this action was not approved: nobody "
                                    "is here to give it; a remembered rule "
                                    "or another approval mode allows it"};
    }
    json review = json::object();
    std::string request_id =
        "approval-" + std::to_string(sequence.fetch_add(1) + 1);
    // Without a preview of its own, a call is approved as what it targets,
    // or as all of its arguments when nothing short names it.
    std::string raw_payload = tool.approval_preview
                                  ? tool.approval_preview(arguments)
                                  : ToolHeadline(&tool, arguments).target;
    if (!tool.approval_preview && raw_payload.empty()) {
      raw_payload = JsonDump(arguments);
    }
    if (decision.effect == ApprovalEffect::kAskModel) {
      if (auto decided = ReviewAutomatically(app, *memo, tool, raw_payload,
                                             turn, review)) {
        verdict = std::move(*decided);
      } else {
        automatic = false;
      }
    }
    if (!automatic) {
      verdict = AskPerson(app, tool, arguments, raw_payload, request_id, root,
                          offered, mandatory, review);
    }
    DebugLog("approval", {{"tool", tool.name},
                          {"automatic", automatic},
                          {"mode", ApprovalModeName(CurrentApprovalMode())},
                          {"session_rule", context.session_rule},
                          {"repository_rule", context.folder_rule},
                          {"review", review},
                          {"mandatory_human", mandatory},
                          {"granted", verdict.granted}});
    Emit(Event{EventId::kApprovalResolved,
               {{"id", request_id},
                {"tool", tool.name},
                {"automatic", automatic},
                {"rule", decisive ? RuleLabel(*decisive) : std::string()},
                {"mode", ApprovalModeName(CurrentApprovalMode())},
                {"review", review},
                {"mandatory_human", mandatory},
                {"granted", verdict.granted}}});
    return verdict.granted ? std::string() : verdict.refusal;
  };
}

}  // namespace uagent
