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
#include "include/app/permissions.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
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

// A mandatory-human decision needs a real person on the other end: a
// terminal or a runtime's clients. Headless runs and piped input cannot
// supply one.
bool InteractiveApprovalAvailable() {
  return isatty(STDIN_FILENO) || InteractiveReadAvailable();
}

namespace {
// What a person may answer an ordinary approval with: the option each client
// offers and every spelling the terminal accepts for it. An answer outside
// the table is guidance: denied, and queued as steering.
enum class ApprovalAnswer { kOnce, kSession, kRepository, kDeny, kGuidance };

struct ApprovalChoice {
  ApprovalAnswer answer;
  std::string_view value;
  // Always names the rule it keeps, and the conversation's the same one; the
  // short answers fit one row of buttons.
  std::string_view label;
  std::array<std::string_view, 3> aliases;
};

constexpr ApprovalChoice kApprovalChoices[] = {
    {ApprovalAnswer::kOnce, "y", "Allow once", {"y", "yes"}},
    {ApprovalAnswer::kSession,
     "s",
     "Allow for this conversation",
     {"s", "session"}},
    {ApprovalAnswer::kRepository,
     "a",
     "Always allow here: ",
     {"a", "always", "repository"}},
    {ApprovalAnswer::kDeny, "n", "Deny", {"n", "no"}},
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

// An agent in Auto that may edit acts inside its folder without review
// while the sandbox holds it there: the commands the sandbox wraps (run and
// scratch, which may also write the sandbox's other roots: temporary
// files and tool caches), and file changes at a path its own check
// finds inside the folder. Stopping a process is not one of them: a
// detached one may belong to another folder.
// Without an enforced sandbox, or for anything else, it is reviewed like
// any Auto session; one that asks, asks.
bool WrapsCommands(const Tool& tool) {
  return tool.name == "run" || tool.name == "scratch";
}

bool ReviewedByPolicy(const AppContext* app, const Tool& tool,
                      const json& arguments) {
  const json agent = app->options.AgentRole();
  return !agent.empty() && JsonValue(agent, "edits", "") != "none" &&
         CurrentApprovalMode() == ApprovalMode::kAuto &&
         SandboxRuntime().mode == SandboxMode::kEnforced &&
         (WrapsCommands(tool) ||
          (tool.needs_approval && !tool.needs_approval(arguments)));
}

// Plan mode only reads. What runs there without asking although it may
// change something: a command the readonly sandbox holds, as for an agent that
// may not edit, and a file tool's call on the plan, the one file it writes.
bool PlanModeAllows(const Tool& tool, const json& arguments, bool mandatory) {
  const SandboxStatus& sandbox = SandboxRuntime();
  if ((tool.capabilities & Capability(ToolCapability::kExecute)) &&
      !mandatory && sandbox.mode == SandboxMode::kEnforced &&
      sandbox.policy.read_only) {
    return true;
  }
  constexpr uint32_t kFiles = Capability(ToolCapability::kInspect) |
                              Capability(ToolCapability::kMutate);
  const std::string plan = PlanFile();
  return !plan.empty() && tool.capabilities != 0 &&
         (tool.capabilities & ~kFiles) == 0 &&
         CanonicalAccessPath(JsonValue(arguments, "path", "")) ==
             CanonicalAccessPath(plan);
}

// The outcome of an approval. A refusal says what was not approved, not who
// refused it: a person may have declined, or a client with nobody behind it
// declined for them.
struct Verdict {
  bool granted = true;
  std::string refusal = "this action was not approved";
};

// What the review made of each action in the turn being run, so the same
// action asked again in that turn is not paid for again. A new turn starts
// afresh.
struct ReviewMemo {
  std::mutex mutex;
  int64_t turn = -1;
  std::unordered_map<std::string, AutoPermissionReview> reviews;
};

// The automatic permission review's verdict, or nothing when it asks for a
// person to decide. Records what it saw in review.
std::optional<Verdict> ReviewAutomatically(AppContext* app, ReviewMemo& memo,
                                           const Tool& tool,
                                           const std::string& raw_payload,
                                           int64_t turn, json& review) {
  Verdict verdict;
  // The latest message, then what the session was asked first: an agent's
  // brief still authorizes what a later note from its parent adds. A cut for
  // length takes the brief's end, never the latest word.
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

// Puts the call to a person, and remembers an answer they ask to keep.
Verdict AskPerson(AppContext* app, const Tool& tool, const json& arguments,
                  const std::string& raw_payload, const std::string& request_id,
                  const std::string& root, const PermissionRule& rule,
                  bool mandatory, const json& review) {
  Verdict verdict;
  // A tool escalates by its path for reaching µAgent's own configuration or
  // one of the person's folders; a tool that escalates for its own reason
  // names it.
  std::string reason =
      tool.mandatory_reason ? tool.mandatory_reason(arguments) : "";
  if (reason.empty()) {
    reason = UnreadablePath(JsonValue(arguments, "path", std::string()))
                 ? "reads your folders outside this folder; sandbox.read "
                   "opens one"
                 : "changes \u00b5Agent's own configuration";
  }
  // What the automatic review made of it, when it was asked: its reasons
  // join the risks, and in words which bar it fell on, or why it could not
  // decide.
  json risks = ApprovalRisks(tool, arguments, root);
  for (const json& caution : JsonValue(review, "reasons", json::array())) {
    risks.push_back(caution);
  }
  std::string verdict_line = JsonValue(review, "error", "");
  verdict_line = verdict_line.empty() ? JsonValue(review, "note", "")
                                      : "could not decide: " + verdict_line;
  // The request carries the full command/payload, so long commands are
  // never truncated. A tool with more to show than its one-line label
  // supplies its own preview.
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
  if (mandatory && !InteractiveApprovalAvailable()) {
    verdict.granted = false;
    verdict.refusal = "this action needs a person's approval (" + reason +
                      ") and nobody is here to give it";
  } else if (mandatory) {
    request.options = json::array({{{"value", "y"}, {"label", "Allow"}},
                                   {{"value", "n"}, {"label", "Deny"}}});
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
    if (chosen == ApprovalAnswer::kRepository) {
      std::string error;
      if (!RememberRepositoryRule(root, rule, error)) {
        Emit(NoticeEvent(
            PresentationStatus::kWarned,
            "allowed once; could not save permission rule: " + error));
      }
    }
    if (chosen == ApprovalAnswer::kGuidance && !answer.empty()) {
      SteeringState().Queue(answer);
    }
  }
  return verdict;
}

}  // namespace

// Approval policy in one place, so the prompt and the yolo shortcut cannot
// drift apart from the debug record of what was granted. A mandatory-human
// call ignores every automatic-approval switch and denies when no human can
// answer. That is defense in depth for the built-in file tools, not a
// boundary: until commands are sandboxed an approved shell reaches the same
// paths with none of these checks in front of it.
Agent::Approver MakeApprover(AppContext* app) {
  auto memo = std::make_shared<ReviewMemo>();
  return [app, memo](const Tool& tool, const json& arguments, int64_t turn) {
    static std::atomic<uint64_t> sequence{0};
    ApprovalClass required = RequiredApproval(tool, arguments);
    bool mandatory = required == ApprovalClass::kMandatoryHuman;
    const std::string root = CanonicalCwd();
    // The rule that allows it, if one does; else the one a person is
    // offered.
    PermissionRule rule;
    const auto covered = [&](const PermissionRule& held) {
      return RuleCovers(held, tool, arguments, root);
    };
    auto held = std::ranges::find_if(app->session_rules, covered);
    bool session_rule = !mandatory && held != app->session_rules.end();
    if (session_rule) rule = *held;
    bool repository_rule = !mandatory && !session_rule &&
                           RepositoryRuleAllows(root, tool, arguments, &rule);
    const bool by_rule = session_rule || repository_rule;
    if (!by_rule) rule = SuggestRule(tool, arguments, root);
    const bool planning = CurrentApprovalMode() == ApprovalMode::kPlan;
    const bool planned = planning && PlanModeAllows(tool, arguments, mandatory);
    bool automatic =
        !mandatory && (ApprovalIsYolo() || session_rule || repository_rule ||
                       ReviewedByPolicy(app, tool, arguments) || planned);
    Verdict verdict;
    // Anything else that changes something is refused before anyone is
    // asked; a read that asks, asks.
    if (planning && !planned && ToolMutates(tool, arguments)) {
      automatic = true;
      verdict = {false,
                 "plan mode is read-only; write the plan to " + PlanFile()};
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
    // The model judges only what the sandbox contains: a call that names a
    // path outside the folder, or a command with no enforced sandbox around
    // it, goes to the person, as a mandatory one does.
    const bool contained = !PathOutsideFolder(arguments, root) &&
                           !(WrapsCommands(tool) &&
                             SandboxRuntime().mode != SandboxMode::kEnforced);
    if (!automatic && !mandatory &&
        CurrentApprovalMode() == ApprovalMode::kAuto && contained) {
      if (auto decided = ReviewAutomatically(app, *memo, tool, raw_payload,
                                             turn, review)) {
        automatic = true;
        verdict = std::move(*decided);
      }
    }
    if (!automatic) {
      verdict = AskPerson(app, tool, arguments, raw_payload, request_id, root,
                          rule, mandatory, review);
    }
    DebugLog("approval", {{"tool", tool.name},
                          {"automatic", automatic},
                          {"mode", ApprovalModeName(CurrentApprovalMode())},
                          {"session_rule", session_rule},
                          {"repository_rule", repository_rule},
                          {"review", review},
                          {"mandatory_human", mandatory},
                          {"granted", verdict.granted}});
    Emit(Event{EventId::kApprovalResolved,
               {{"id", request_id},
                {"tool", tool.name},
                {"automatic", automatic},
                {"rule", by_rule ? RuleLabel(rule) : std::string()},
                {"mode", ApprovalModeName(CurrentApprovalMode())},
                {"review", review},
                {"mandatory_human", mandatory},
                {"granted", verdict.granted}}});
    return verdict.granted ? std::string() : verdict.refusal;
  };
}

}  // namespace uagent
