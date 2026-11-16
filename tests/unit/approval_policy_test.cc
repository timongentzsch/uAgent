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
  CHECK(Is(Decide(EditInFolder(), In(Mode::kAuto)), kAskModel, kReview));
  CHECK(Is(Decide(Fetch(), In(Mode::kAuto)), kAskModel, kReview));
  CHECK(Is(Decide(EditOutside(), In(Mode::kAuto)), kAskPerson, kMode));
  CHECK(Is(Decide(ReadOutside(), In(Mode::kAuto)), kAskPerson, kMode));
  PolicyContext loose = In(Mode::kAuto);
  loose.sandbox_enforced = false;
  CHECK(Is(Decide(Command(), loose), kAskPerson, kMode));
  CHECK(Is(Decide(EditInFolder(), loose), kAskModel, kReview));

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

  // An agent that may edit, in Auto, runs commands and in-folder edits
  // unreviewed while the sandbox is enforced.
  PolicyContext agent = In(Mode::kAuto);
  agent.agent_edits = true;
  CHECK(Is(Decide(Command(), agent), kAllow, kAgentPolicy));
  CHECK(Is(Decide(EditInFolder(), agent), kAllow, kAgentPolicy));
  CHECK(Is(Decide(Fetch(), agent), kAskModel, kReview));
  CHECK(Is(Decide(EditOutside(), agent), kAskPerson, kMode));
  agent.sandbox_enforced = false;
  CHECK(Is(Decide(Command(), agent), kAskPerson, kMode));

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
    for (uint32_t bits = 0; bits < (1u << 13); ++bits) {
      const auto bit = [bits](int at) { return ((bits >> at) & 1u) != 0; };
      const ApprovalFacts call{
          .required = bit(0) ? kMandatoryCall : kOrdinaryCall,
          .mutates = bit(1),
          .executes = bit(2),
          .command = bit(3),
          .on_plan_file = bit(4),
          .inside_work_area = bit(5),
          .outside_folder = bit(6)};
      const PolicyContext context{.mode = mode,
                                  .sandbox_enforced = bit(7),
                                  .sandbox_read_only = bit(8),
                                  .agent_edits = bit(9),
                                  .person = bit(12),
                                  .session_rule = bit(10),
                                  .folder_rule = bit(11)};
      const ApprovalDecision decision = Decide(call, context);
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
      // The model sees only Auto's contained calls.
      if (decision.effect == kAskModel) {
        CHECK(mode == Mode::kAuto && call.required != kMandatoryCall &&
              !call.outside_folder &&
              (!call.command || context.sandbox_enforced));
      }
      // Yolo never asks the model, and asks a person only when it must.
      if (mode == Mode::kYolo) {
        CHECK(decision.effect != kAskModel);
        if (decision.effect == kAskPerson) {
          CHECK(call.required == kMandatoryCall);
        }
      }
    }
  }
}

}  // namespace uagent
