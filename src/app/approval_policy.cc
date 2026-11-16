// Copyright 2026 Timon Gentzsch

#include "include/app/approval_policy.h"

namespace uagent {

ApprovalDecision Decide(const ApprovalFacts& call,
                        const PolicyContext& context) {
  using enum ApprovalEffect;
  using enum ApprovalSource;
  const bool mandatory = call.required == ApprovalClass::kMandatoryHuman;
  const bool planning = context.mode == ApprovalMode::kPlan;
  const bool automatic = context.mode == ApprovalMode::kAuto;
  // With nobody to ask, what a person would decide is refused: an unattended
  // run never takes its answers from whatever its input holds.
  const auto ask = [&context](ApprovalSource source) -> ApprovalDecision {
    return context.person ? ApprovalDecision{kAskPerson, source}
                          : ApprovalDecision{kDeny, kNobody};
  };
  // Plan mode only reads. What runs there although it may change something:
  // a command the read-only sandbox holds, and a call on the plan file.
  const bool plan_allows =
      planning && (call.on_plan_file ||
                   (call.executes && !mandatory && context.sandbox_enforced &&
                    context.sandbox_read_only));
  // A rule that refuses holds in every mode, before anyone is asked.
  if (context.deny_rule) return {kDeny, kDenyRule};
  // Anything else that changes something is refused before anyone is asked.
  if (planning && !plan_allows && call.mutates) return {kDeny, kPlan};
  // A mandatory call ignores every automatic-approval switch.
  if (mandatory) return ask(kMandatory);
  if (context.ask_rule) return ask(kAskRule);
  if (context.session_rule) return {kAllow, kSessionRule};
  if (context.folder_rule) return {kAllow, kFolderRule};
  if (context.mode == ApprovalMode::kYolo) return {kAllow, kYolo};
  if (plan_allows) return {kAllow, kPlanAllows};
  // In Auto a file tool's change inside the folder needs no judge: undo
  // takes it back. Commands and the rest are judged, in an agent as in the
  // conversation that started it.
  if (automatic && call.inside_work_area && !call.executes &&
      !call.outside_folder) {
    return {kAllow, kContainedEdit};
  }
  // The model judges only what the sandbox contains: nothing outside the
  // folder, no command without the enforced sandbox around it.
  const bool confined = !call.command || context.sandbox_enforced;
  if (automatic && confined && !call.outside_folder) {
    return context.review_paused ? ask(kRefusals)
                                 : ApprovalDecision{kAskModel, kReview};
  }
  return ask(kMode);
}

const char* ApprovalSourceName(ApprovalSource source) {
  switch (source) {
    using enum ApprovalSource;
    case kDenyRule:
      return "deny_rule";
    case kPlan:
      return "plan";
    case kMandatory:
      return "mandatory";
    case kAskRule:
      return "ask_rule";
    case kSessionRule:
      return "session_rule";
    case kFolderRule:
      return "folder_rule";
    case kYolo:
      return "yolo";
    case kPlanAllows:
      return "plan_allows";
    case kContainedEdit:
      return "contained_edit";
    case kReview:
      return "review";
    case kRefusals:
      return "refusals";
    case kMode:
      return "mode";
    case kNobody:
      return "nobody";
  }
  return "";
}

}  // namespace uagent
