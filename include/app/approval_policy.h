// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_APPROVAL_POLICY_H_
#define UAGENT_INCLUDE_APP_APPROVAL_POLICY_H_
// Who or what decides a tool call that needs approval. Pure: the driver
// (src/app/approvals.cc) gathers the facts, carries out the effect, and one
// table test holds the whole order.

#include "include/core/env.h"
#include "include/tools/tool.h"

namespace uagent {

// What the call is.
struct ApprovalFacts {
  ApprovalClass required = ApprovalClass::kYoloEligibleMutation;
  bool mutates = false;
  // It runs commands (the execute capability).
  bool executes = false;
  // A `run` call: a rule and a review hold it only in the enforced sandbox.
  bool command = false;
  // A file tool's call on the plan file.
  bool on_plan_file = false;
  // A path tool whose path lies in the work area.
  bool inside_work_area = false;
  // It names a path outside the folder.
  bool outside_folder = false;
};

// Where it is decided.
struct PolicyContext {
  ApprovalMode mode = ApprovalMode::kAsk;
  bool sandbox_enforced = false;
  bool sandbox_read_only = false;
  // A person can answer: a terminal, or a runtime's clients.
  bool person = true;
  // A folder rule holds the call back: refuses it, or asks a person.
  bool deny_rule = false;
  bool ask_rule = false;
  // A remembered rule lets it through: this conversation's, or the folder's.
  bool session_rule = false;
  bool folder_rule = false;
};

enum class ApprovalEffect { kAllow, kDeny, kAskPerson, kAskModel };

// The stage of the order that decided.
enum class ApprovalSource {
  kDenyRule,
  kPlan,
  kMandatory,
  kAskRule,
  kSessionRule,
  kFolderRule,
  kYolo,
  kPlanAllows,
  kContainedEdit,
  kReview,
  kMode,
  // A person would decide, and nobody can.
  kNobody,
};

struct ApprovalDecision {
  ApprovalEffect effect;
  ApprovalSource source;
};

ApprovalDecision Decide(const ApprovalFacts& call,
                        const PolicyContext& context);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_APPROVAL_POLICY_H_
