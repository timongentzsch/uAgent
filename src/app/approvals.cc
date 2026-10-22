// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "include/agent/session_role.h"
#include "include/app/panel.h"
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

// A mandatory-human decision needs a real person on the other end. Headless
// runs, delegated children and piped input cannot supply one.
bool InteractiveApprovalAvailable() {
  return (isatty(STDIN_FILENO) || InteractiveReadAvailable()) &&
         AgentDepth() == 0;
}

namespace {
// What a person may answer an ordinary approval with: the option each client
// offers and every spelling the terminal accepts for it. An answer outside
// the table is guidance: denied, and queued as steering.
enum class ApprovalAnswer { kOnce, kSession, kRepository, kDeny, kGuidance };

struct ApprovalChoice {
  ApprovalAnswer answer;
  std::string_view value;
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
     "Always allow this exact action here",
     {"a", "always", "repository"}},
    {ApprovalAnswer::kDeny, "n", "Deny", {"n", "no"}},
    {ApprovalAnswer::kGuidance, "guidance", "Send guidance", {}},
};

json ApprovalOptions() {
  json options = json::array();
  for (const ApprovalChoice& choice : kApprovalChoices) {
    options.push_back({{"value", choice.value}, {"label", choice.label}});
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

// A panel's thread acts inside its folder without review while
// the sandbox holds it there: the commands the sandbox wraps (run and
// scratch, which may also write the sandbox's other roots: temporary
// files and tool caches), and file changes at a path its own check
// finds inside the workspace. Stopping a process is not one of them: a
// detached one may belong to another folder.
// Without an enforced sandbox, or for anything else, it is reviewed like
// any Auto session. A chat member reads what was shared in its panel's chat as
// it reads the folder: without review.
bool WrapsCommands(const Tool& tool) {
  return tool.name == "run" || tool.name == "scratch";
}

bool ReviewedByPolicy(const AppContext* app, const Tool& tool,
                      const json& arguments) {
  const bool confined_thread =
      app->options.Thread() &&
      SandboxRuntime().mode == SandboxMode::kEnforced &&
      (WrapsCommands(tool) ||
       (tool.needs_approval && !tool.needs_approval(arguments)));
  const json thread = ThreadRole(app->options.session);
  const bool shared_file =
      tool.name == "read_path" && !ChatMember(thread).empty() &&
      PathWithin(
          CanonicalAccessPath(JsonValue(arguments, "path", "")),
          CanonicalAccessPath(PanelPath(JsonValue(thread, "folder", "")) +
                              kSessionAssetsSuffix));
  return confined_thread || shared_file;
}

// The outcome of an approval: who refused, for the model, is a person by
// default.
struct Verdict {
  bool granted = true;
  std::string refusal = "user denied this action";
};

// The automatic permission review's verdict, or nothing when it asks for a
// person to decide. Records what it saw in review.
std::optional<Verdict> ReviewAutomatically(AppContext* app, const Tool& tool,
                                           const std::string& raw_payload,
                                           int64_t turn, json& review) {
  Verdict verdict;
  // The latest message, then what the session was asked first: a thread's
  // brief still authorizes what a later note from its panel adds. A cut for
  // length takes the brief's end, never the latest word.
  const Conversation& history = app->agent->History();
  std::string request = history.UserMessageText(history.UserTurns());
  if (const std::string first = history.UserMessageText(1); first != request) {
    request += "\n\n[first message]\n" + first;
  }
  AutoPermissionReview decision = ReviewPermission(
      app->runtime.permission_api,
      ResolveReviewRoute(app->runtime.api, app->provider.providers,
                         app->runtime.config.permission_model),
      app->runtime.config, app->runtime.side_usage,
      {tool, raw_payload, request, app->agent->SessionId(), turn});
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
                  const std::string& root, const std::string& key,
                  bool mandatory, const json& review) {
  Verdict verdict;
  // Reaching µAgent's own configuration is the reason a tool escalates by
  // its path; a tool that escalates for its own reason names it.
  std::string reason = tool.mandatory_reason.empty()
                           ? "changes \u00b5Agent's own configuration"
                           : tool.mandatory_reason;
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
  } else {
    request.options = ApprovalOptions();
    bool cancelled = false;
    bool eof = false;
    std::string answer =
        Trim(ReadChoiceLine(std::move(request), cancelled, eof));
    const ApprovalAnswer chosen =
        cancelled || eof ? ApprovalAnswer::kDeny : ParseApprovalAnswer(answer);
    verdict.granted = chosen == ApprovalAnswer::kOnce ||
                      chosen == ApprovalAnswer::kSession ||
                      chosen == ApprovalAnswer::kRepository;
    if (chosen == ApprovalAnswer::kSession) {
      app->session_approvals.insert(key);
    }
    if (chosen == ApprovalAnswer::kRepository) {
      std::string error;
      if (!RememberRepositoryPermission(root, key, tool.name, raw_payload,
                                        error)) {
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
  return [app](const Tool& tool, const json& arguments, int64_t turn) {
    static std::atomic<uint64_t> sequence{0};
    ApprovalClass required = RequiredApproval(tool, arguments);
    bool mandatory = required == ApprovalClass::kMandatoryHuman;
    std::string key = PermissionKey(tool, arguments, required);
    const std::string root = CanonicalCwd();
    bool session_rule = !mandatory && app->session_approvals.contains(key);
    bool repository_rule =
        !mandatory && !session_rule && RepositoryPermissionAllows(root, key);
    bool automatic =
        !mandatory && (ApprovalIsYolo() || session_rule || repository_rule ||
                       ReviewedByPolicy(app, tool, arguments));
    Verdict verdict;
    json review = json::object();
    std::string request_id =
        "approval-" + std::to_string(sequence.fetch_add(1) + 1);
    const std::string raw_payload = tool.approval_preview
                                        ? tool.approval_preview(arguments)
                                        : ToolSummary(tool, arguments);
    // The model judges only what the sandbox contains: a call that names a
    // path outside the folder, or a command with no enforced sandbox around
    // it, goes to the person, as a mandatory one does.
    const bool contained = !PathOutsideFolder(arguments, root) &&
                           !(WrapsCommands(tool) &&
                             SandboxRuntime().mode != SandboxMode::kEnforced);
    if (!automatic && !mandatory &&
        CurrentApprovalMode() == ApprovalMode::kAuto && contained) {
      if (auto decided =
              ReviewAutomatically(app, tool, raw_payload, turn, review)) {
        automatic = true;
        verdict = std::move(*decided);
      }
    }
    if (!automatic) {
      verdict = AskPerson(app, tool, arguments, raw_payload, request_id, root,
                          key, mandatory, review);
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
                {"mode", ApprovalModeName(CurrentApprovalMode())},
                {"review", review},
                {"mandatory_human", mandatory},
                {"granted", verdict.granted}}});
    return verdict.granted ? std::string() : verdict.refusal;
  };
}

}  // namespace uagent
