// Copyright 2026 Timon Gentzsch

#include "include/app/approval_policy.h"

#include <array>
#include <cstdint>

#include "tests/unit/test_support.h"

namespace uagent {
namespace {

using enum ApprovalEffect;
using enum ApprovalSource;
using Mode = ApprovalMode;

constexpr ApprovalClass kOrdinaryCall = ApprovalClass::kYoloEligibleMutation;
constexpr ApprovalClass kMandatoryCall = ApprovalClass::kMandatoryHuman;

// Calls of the kinds that need approval.
ApprovalFacts Command() {
  return {.required = kOrdinaryCall,
          .mutates = true,
          .executes = true,
          .command = true};
}
ApprovalFacts EditInFolder() {
  return {.required = kOrdinaryCall, .mutates = true, .inside_work_area = true};
}
ApprovalFacts EditOutside() {
  return {.required = kOrdinaryCall, .mutates = true, .outside_folder = true};
}
ApprovalFacts Fetch() { return {.required = kOrdinaryCall}; }
ApprovalFacts ReadOutside() {
  return {.required = kOrdinaryCall, .outside_folder = true};
}
ApprovalFacts Unsandboxed() {
  ApprovalFacts call = Command();
  call.required = kMandatoryCall;
  return call;
}
ApprovalFacts Delegation() {
  return {.required = kOrdinaryCall, .delegates = true};
}
ApprovalFacts PlanWrite() {
  ApprovalFacts call = EditInFolder();
  call.on_plan_file = true;
  return call;
}

PolicyContext In(Mode mode) { return {.mode = mode, .sandbox_enforced = true}; }

bool Is(const ApprovalDecision& decision, ApprovalEffect effect,
        ApprovalSource source) {
  return decision.effect == effect && decision.source == source;
}

}  // namespace

// Today's order, case by case.
TEST_CASE(TestApprovalOrder) {
  // Ask: a person decides what no rule covers.
  CHECK(Is(Decide(Command(), In(Mode::kAsk)), kAskPerson, kMode));
  CHECK(Is(Decide(EditInFolder(), In(Mode::kAsk)), kAskPerson, kMode));
  CHECK(Is(Decide(Fetch(), In(Mode::kAsk)), kAskPerson, kMode));

  // Auto: the model judges what the sandbox contains, a person the rest.
  CHECK(Is(Decide(Command(), In(Mode::kAuto)), kAskModel, kReview));
  CHECK(Is(Decide(EditInFolder(), In(Mode::kAuto)), kAllow, kContainedEdit));
  CHECK(Is(Decide(Fetch(), In(Mode::kAuto)), kAskModel, kReview));
  CHECK(Is(Decide(EditOutside(), In(Mode::kAuto)), kAskPerson, kMode));
  CHECK(Is(Decide(ReadOutside(), In(Mode::kAuto)), kAskPerson, kMode));
  PolicyContext loose = In(Mode::kAuto);
  loose.sandbox_enforced = false;
  CHECK(Is(Decide(Command(), loose), kAskPerson, kMode));
  CHECK(Is(Decide(EditInFolder(), loose), kAllow, kContainedEdit));

  // An agent's brief is judged in Auto, sandbox or not, and needs nobody in
  // the other modes; rules hold it back as any call.
  CHECK(Is(Decide(Delegation(), In(Mode::kAuto)), kAskModel, kReview));
  CHECK(Is(Decide(Delegation(), loose), kAskModel, kReview));
  CHECK(Is(Decide(Delegation(), In(Mode::kAsk)), kAllow, kDelegation));
  CHECK(Is(Decide(Delegation(), In(Mode::kPlan)), kAllow, kDelegation));
  CHECK(Is(Decide(Delegation(), In(Mode::kYolo)), kAllow, kYolo));
  PolicyContext asking = In(Mode::kAsk);
  asking.ask_rule = true;
  CHECK(Is(Decide(Delegation(), asking), kAskPerson, kAskRule));
  asking.deny_rule = true;
  CHECK(Is(Decide(Delegation(), asking), kDeny, kDenyRule));

  // With nobody to ask, what a person would decide is refused; what is
  // decided without one is not.
  PolicyContext alone = In(Mode::kAsk);
  alone.person = false;
  CHECK(Is(Decide(Command(), alone), kDeny, kNobody));
  CHECK(Is(Decide(Unsandboxed(), alone), kDeny, kNobody));
  alone.folder_rule = true;
  CHECK(Is(Decide(Command(), alone), kAllow, kFolderRule));
  alone = In(Mode::kAuto);
  alone.person = false;
  CHECK(Is(Decide(Command(), alone), kAskModel, kReview));
  CHECK(Is(Decide(EditOutside(), alone), kDeny, kNobody));

  // After the review's refusals, Auto asks a person what it would judge.
  PolicyContext paused = In(Mode::kAuto);
  paused.review_paused = true;
  CHECK(Is(Decide(Command(), paused), kAskPerson, kRefusals));
  CHECK(Is(Decide(EditInFolder(), paused), kAllow, kContainedEdit));

  // A rule that refuses holds in every mode and before a person is asked;
  // one that asks, before anything lets the call through.
  for (const Mode mode : {Mode::kAsk, Mode::kAuto, Mode::kYolo, Mode::kPlan}) {
    PolicyContext held = In(mode);
    held.deny_rule = held.ask_rule = held.folder_rule = true;
    CHECK(Is(Decide(Command(), held), kDeny, kDenyRule));
    CHECK(Is(Decide(Unsandboxed(), held), kDeny, kDenyRule));
    held.deny_rule = false;
    if (mode != Mode::kPlan) {
      CHECK(Is(Decide(Command(), held), kAskPerson, kAskRule));
      CHECK(Is(Decide(Unsandboxed(), held), kAskPerson, kMandatory));
    }
  }

  // Yolo allows everything but the mandatory class.
  CHECK(Is(Decide(Command(), In(Mode::kYolo)), kAllow, kYolo));
  CHECK(Is(Decide(EditOutside(), In(Mode::kYolo)), kAllow, kYolo));
  CHECK(Is(Decide(Unsandboxed(), In(Mode::kYolo)), kAskPerson, kMandatory));

  // Rules allow, the conversation's first; never a mandatory call.
  PolicyContext ruled = In(Mode::kAsk);
  ruled.session_rule = ruled.folder_rule = true;
  CHECK(Is(Decide(Command(), ruled), kAllow, kSessionRule));
  ruled.session_rule = false;
  CHECK(Is(Decide(Command(), ruled), kAllow, kFolderRule));
  CHECK(Is(Decide(Unsandboxed(), ruled), kAskPerson, kMandatory));

  // Plan mode refuses changes, first of all, except a command the read-only
  // sandbox holds and the plan file.
  PolicyContext plan = In(Mode::kPlan);
  plan.sandbox_read_only = true;
  CHECK(Is(Decide(Command(), plan), kAllow, kPlanAllows));
  CHECK(Is(Decide(PlanWrite(), plan), kAllow, kPlanAllows));
  CHECK(Is(Decide(EditInFolder(), plan), kDeny, kPlan));
  CHECK(Is(Decide(Unsandboxed(), plan), kDeny, kPlan));
  plan.folder_rule = true;
  CHECK(Is(Decide(EditInFolder(), plan), kDeny, kPlan));
  plan.folder_rule = false;
  CHECK(Is(Decide(ReadOutside(), plan), kAskPerson, kMode));
  plan.sandbox_read_only = false;
  CHECK(Is(Decide(Command(), plan), kDeny, kPlan));
}

// What holds for every combination of facts and situation.
TEST_CASE(TestApprovalInvariants) {
  constexpr std::array kModes = {Mode::kAsk, Mode::kAuto, Mode::kYolo,
                                 Mode::kPlan};
  for (const Mode mode : kModes) {
    for (uint32_t bits = 0; bits < (1u << 16); ++bits) {
      const auto bit = [bits](int at) { return ((bits >> at) & 1u) != 0; };
      const ApprovalFacts call{
          .required = bit(0) ? kMandatoryCall : kOrdinaryCall,
          .mutates = bit(1),
          .executes = bit(2),
          .command = bit(3),
          .on_plan_file = bit(4),
          .inside_work_area = bit(5),
          .outside_folder = bit(6),
          .delegates = bit(15)};
      const PolicyContext context{.mode = mode,
                                  .sandbox_enforced = bit(7),
                                  .sandbox_read_only = bit(8),
                                  .person = bit(12),
                                  .deny_rule = bit(13),
                                  .ask_rule = bit(14),
                                  .session_rule = bit(10),
                                  .folder_rule = bit(11),
                                  .review_paused = bit(9)};
      const ApprovalDecision decision = Decide(call, context);
      // A rule that refuses always does; one that asks is never passed by.
      if (context.deny_rule) CHECK(decision.effect == kDeny);
      if (context.ask_rule && decision.effect == kAllow) {
        CHECK(mode == Mode::kPlan);
      }
      // Nobody to ask is never asked.
      if (!context.person) CHECK(decision.effect != kAskPerson);
      // A mandatory call is never allowed without a person.
      if (call.required == kMandatoryCall) CHECK(decision.effect != kAllow);
      // Plan mode allows a change only on the plan file or in the read-only
      // sandbox.
      if (mode == Mode::kPlan && call.mutates && decision.effect == kAllow) {
        CHECK(call.on_plan_file || (call.executes && context.sandbox_enforced &&
                                    context.sandbox_read_only));
      }
      // Auto lets through unjudged only a file tool's change in the folder.
      if (mode == Mode::kAuto && decision.source == kContainedEdit) {
        CHECK(call.inside_work_area && !call.executes && !call.outside_folder);
      }
      // The model sees only Auto's contained calls and its delegations,
      // while not paused.
      if (decision.effect == kAskModel) {
        CHECK(!context.review_paused);
        CHECK(mode == Mode::kAuto && call.required != kMandatoryCall);
        CHECK(call.delegates || (!call.outside_folder &&
                                 (!call.command || context.sandbox_enforced)));
      }
      // A delegation is asked of a person outside Auto only by a rule or as
      // a mandatory call.
      if (call.delegates && mode != Mode::kAuto &&
          decision.effect == kAskPerson) {
        CHECK(context.ask_rule || call.required == kMandatoryCall);
      }
      // Yolo never asks the model, and a person only for a mandatory call or
      // one a rule holds back.
      if (mode == Mode::kYolo) {
        CHECK(decision.effect != kAskModel);
        if (decision.effect == kAskPerson) {
          CHECK(call.required == kMandatoryCall || context.ask_rule);
        }
      }
    }
  }
}

}  // namespace uagent
