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
  // Anything else that changes something is refused before anyone is asked.
  if (planning && !plan_allows && call.mutates) return {kDeny, kPlan};
  // A mandatory call ignores every automatic-approval switch.
  if (mandatory) return ask(kMandatory);
  if (context.session_rule) return {kAllow, kSessionRule};
  if (context.folder_rule) return {kAllow, kFolderRule};
  if (context.mode == ApprovalMode::kYolo) return {kAllow, kYolo};
  // An agent that may edit acts in its folder while the enforced sandbox
  // holds it there: commands, and path tools inside the work area.
  if (context.agent_edits && automatic && context.sandbox_enforced &&
      (call.command || call.inside_work_area)) {
    return {kAllow, kAgentPolicy};
  }
  if (plan_allows) return {kAllow, kPlanAllows};
  // The model judges only what the sandbox contains: nothing outside the
  // folder, no command without the enforced sandbox around it.
  const bool confined = !call.command || context.sandbox_enforced;
  if (automatic && confined && !call.outside_folder) {
    return {kAskModel, kReview};
  }
  return ask(kMode);
}

}  // namespace uagent
