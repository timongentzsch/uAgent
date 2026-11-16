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
    {ApprovalAnswer::kDeny, kDenyOnce, "Deny", {kDenyOnce, "no"}},
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
  std::string reason =
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

// After this many refusals by the review, in a row or in the conversation,
// Auto asks a person until one answers.
constexpr int kReviewRefusalsInRow = 3;
constexpr int kReviewRefusals = 20;

// The review of each action in the turn being run, so the same action asked
// again in that turn is not paid for again; and its refusals.
struct ReviewMemo {
  std::mutex mutex;
  int64_t turn = -1;
  std::unordered_map<std::string, AutoPermissionReview> reviews;
  int refused_in_row = 0;
  int refused = 0;

  bool Paused() {
    std::lock_guard lock(mutex);
    return refused_in_row >= kReviewRefusalsInRow || refused >= kReviewRefusals;
  }
  // A person answered: the review judges again.
  void Answered() {
    std::lock_guard lock(mutex);
    refused_in_row = refused = 0;
  }
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
  const bool ask = decision.decision == AutoPermissionDecision::kAsk;
  const bool allow = decision.decision == AutoPermissionDecision::kAllow;
  {
    std::lock_guard lock(memo.mutex);
    if (!memoized && decision.error.empty() && memo.turn == turn) {
      memo.reviews.emplace(memo_key, decision);
    }
    if (allow) memo.refused_in_row = 0;
    if (!allow && !ask) {
      ++memo.refused_in_row;
      ++memo.refused;
    }
  }
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

// Why a person is asked, in a line: the stage of the order that asked.
std::string AskReason(ApprovalSource source, const ApprovalFacts& facts,
                      const PolicyContext& context, const std::string& rule,
                      const std::string& mandatory, const json& review,
                      bool in_row) {
  switch (source) {
    case ApprovalSource::kMandatory:
      return "Needs you in every mode: " + mandatory + ".";
    case ApprovalSource::kAskRule:
      return "A rule of this folder asks for it: " + rule + ".";
    case ApprovalSource::kReview: {
      const std::string error = JsonValue(review, "error", "");
      const std::string note = JsonValue(review, "note", "");
      return error.empty()
                 ? "Auto review " + (note.empty() ? "could not decide." : note)
                 : "Auto review could not decide: " + error;
    }
    case ApprovalSource::kRefusals:
      return in_row ? "Auto review refused " +
                          std::to_string(kReviewRefusalsInRow) +
                          " calls in a row; it asks you until you answer one."
                    : "Auto review refused " + std::to_string(kReviewRefusals) +
                          " calls in this conversation; it asks you until "
                          "you answer one.";
    default:
      break;
  }
  if (context.mode == ApprovalMode::kAuto) {
    return facts.outside_folder
               ? "Auto asks you about anything outside this folder."
               : "Auto asks you about commands while the sandbox is off.";
  }
  if (context.mode == ApprovalMode::kPlan) {
    return "Plan mode asks before anything but reading.";
  }
  return "The approval mode is Ask.";
}

Verdict AskPerson(AppContext* app, const Tool& tool, const json& arguments,
                  const std::string& raw_payload, const std::string& request_id,
                  const std::string& root, const PermissionRule& rule,
                  ApprovalSource source, const std::string& reason,
                  const std::string& asking_rule, const json& review) {
  Verdict verdict;
  const bool mandatory = source == ApprovalSource::kMandatory;
  // The review's reasons join the risks.
  json risks = ApprovalRisks(tool, arguments, root);
  for (const json& caution : JsonValue(review, "reasons", json::array())) {
    risks.push_back(caution);
  }
  // The preview is the whole payload: a long command is never truncated.
  Emit(Event{EventId::kApprovalRequested,
             {{"id", request_id},
              {"tool", tool.name},
              {"preview", TerminalSafe(raw_payload)},
              {"risks", std::move(risks)},
              {"source", ApprovalSourceName(source)},
              {"reason", reason},
              {"rule", asking_rule}}});
  InteractionRequest request{
      .id = request_id,
      .kind = "approval",
      .prompt = "Allow " + TerminalSafe(tool.name) + "?"};
  if (mandatory) {
    request.options = json::array({{{"value", kAllowOnce}, {"label", "Allow"}},
                                   {{"value", kDenyOnce}, {"label", "Deny"}}});
    verdict.granted = Confirm(std::move(request));
    verdict.refusal += " (" + MandatoryReason(tool, arguments) + ")";
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
    if (chosen == ApprovalAnswer::kSession) app->session_rules.Add(rule);
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
  return [app, memo](const Tool& tool, const json& arguments,
                     int64_t turn) -> ApprovalOutcome {
    static std::atomic<uint64_t> sequence{0};
    const std::string root = Workspace().path;
    const SandboxStatus& sandbox = SandboxRuntime();
    PolicyContext context{
        .mode = CurrentApprovalMode(),
        .sandbox_enforced = sandbox.mode == SandboxMode::kEnforced,
        .sandbox_read_only = sandbox.policy.read_only,
        .person = InteractiveApprovalAvailable(),
        .review_paused = memo->Paused()};
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
      facts.inside_work_area =
          tool.needs_approval && !tool.needs_approval(arguments);
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
      allowing = app->session_rules.Covering(tool, arguments, root, confined);
      if (allowing) {
        context.session_rule = true;
      } else if (folder.allow) {
        context.folder_rule = true;
        allowing = folder.allow;
      }
    }
    // What a person is offered to remember.
    const PermissionRule offered = SuggestRule(tool, arguments, root, confined);
    const ApprovalDecision decision = Decide(facts, context);
    ApprovalSource source = decision.source;
    const std::optional<PermissionRule>& decisive =
        source == ApprovalSource::kDenyRule  ? folder.deny
        : source == ApprovalSource::kAskRule ? folder.ask
                                             : allowing;
    const std::string rule = decisive ? RuleLabel(*decisive) : std::string();
    bool automatic = decision.effect != ApprovalEffect::kAskPerson;
    Verdict verdict;
    if (source == ApprovalSource::kDenyRule) {
      verdict = {false,
                 "this action was not approved: a rule of this folder "
                 "refuses it (" +
                     rule + ")"};
    } else if (source == ApprovalSource::kPlan) {
      verdict = {false,
                 "plan mode is read-only; write the plan to " + PlanFile()};
    } else if (source == ApprovalSource::kNobody) {
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
      const bool in_row = [&memo] {
        std::lock_guard lock(memo->mutex);
        return memo->refused_in_row >= kReviewRefusalsInRow;
      }();
      verdict = AskPerson(
          app, tool, arguments, raw_payload, request_id, root, offered, source,
          AskReason(source, facts, context, rule,
                    MandatoryReason(tool, arguments), review, in_row),
          source == ApprovalSource::kAskRule ? rule : std::string(), review);
      memo->Answered();
    }
    const char* effect = verdict.granted ? "allow" : "deny";
    DebugLog("approval", {{"tool", tool.name},
                          {"effect", effect},
                          {"source", ApprovalSourceName(source)},
                          {"asked", !automatic},
                          {"mode", ApprovalModeName(CurrentApprovalMode())},
                          {"review", review}});
    Emit(Event{EventId::kApprovalResolved,
               {{"id", request_id},
                {"tool", tool.name},
                {"effect", effect},
                {"source", ApprovalSourceName(source)},
                {"asked", !automatic},
                {"reason", verdict.granted ? std::string() : verdict.refusal},
                {"rule", rule},
                {"mode", ApprovalModeName(CurrentApprovalMode())},
                {"review", review}}});
    if (verdict.granted) return {};
    // A refusal nobody was asked about shows on its row, with what lets the
    // call through: the action itself for this conversation, or the rules.
    json part = nullptr;
    if (automatic && (source == ApprovalSource::kReview ||
                      source == ApprovalSource::kDenyRule)) {
      part = {{"kind", "refusal"},
              {"source", ApprovalSourceName(source)},
              {"reason", source == ApprovalSource::kDenyRule
                             ? "A rule of this folder refuses it: " + rule
                             : AskReason(source, facts, context, rule, "",
                                         review, false)},
              {"rule", rule}};
      if (source == ApprovalSource::kReview) {
        part["offer"] = {{"tool", tool.name},
                         {"match", RuleSubject(tool, arguments, root)}};
      }
    }
    return {verdict.refusal, std::move(part)};
  };
}

}  // namespace uagent
