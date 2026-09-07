---
name: self-improve
description: Run one bounded source-improvement attempt on µAgent, or operate its measured incumbent/successor comparison, promotion and rollback workflow.
argument-hint: [attempt|status|start|verdict GENERATION|rollback]
requires-tools: run, read_path, edit_file, grep
---

# Self-improve

For a source-improvement attempt, follow `${SKILL_DIR}/INSTRUCTION.md`. The
controller supplies the source, fixed instruction, route, limits and existing
gates. Stop after one justified candidate or no validated improvement.

Record `.uagent-improvement.json` before the substantive edit:

```json
{
  "schema": "uagent.improvement.claim.v1",
  "hypothesis": "one falsifiable sentence",
  "measurement": "what observable behavior will improve",
  "verify_command": "the command that demonstrates the improvement",
  "assessment": {
    "change_summary": "what changed and why",
    "impact": "observed before/after results",
    "generality": "expected scope of benefit and supporting reasoning",
    "limitations": "untested cases, risks and missing evidence",
    "recommendation": "propose",
    "proposed_title": "suggested commit or PR title"
  }
}
```

The controller requires the check to exit 0 on the candidate and 1 on the
original source. New focused measurement files can be added, but existing
verifier files and protected inputs must stay byte-identical. A passing check
alone does not establish a useful improvement: explain why its result matters.
Complete `assessment` after verification; recommendation is `propose`, `revise`
or `reject`. Generality is the agent's judgment, not an independently measured
result. Claim-before-edit is instructed, not enforced by the controller.

For an operator request, run `python3 ${SKILL_DIR}/scripts/experiment.py --help`
and the relevant subcommand's `--help`. The sequence is `init`, `discover`,
`gate`, `review`. `discover` first builds and tests the unchanged source; a
`preflight_failed` result stops before model calls and is not a candidate verdict.
Use the pinned controller path returned
by `init`, an explicitly selected executor/skill bundle, frozen route config,
and reviewed route authority and limits. Never use normal subagent delegation
to choose the executor version.
Model calls are unlimited by default; for a subscription route, also declare
`limits.max_model_calls: 0` in its authority. Other run limits still apply.
For trusted local µAgent runs on macOS, select `--gate-mode host` at `init` so
the full test suite can exercise its own sandboxes. Verification uses clean
temporary source copies and HOME, but host mode runs candidate code with user
permissions. Executors remain sandboxed. Never retry a failed sandbox gate in
host mode within the same generation.
`source_validated` records a proven source fix independently of the later A/B
verdict; a useful fix need not establish better recursive self-improvement.

`review` produces `review.md`, `review.json` and `proposal.patch`. Read and
present the change, measured impact, agent-assessed generality and limitations,
recommendation, and proposed title to the human, with links to the exact diff
and evidence. Independently inspect the measurement and relevant edge cases;
do not treat the author's check or recommendation as an independent oracle.
Request authorization for the specific apply/commit/PR action only after this
presentation, unless the human already authorized that action for this exact
reviewed proposal. Review does not authorize publication or activation. The
controller has no commit or PR command; this boundary is an operator workflow
requirement, not an OS-enforced restriction on Git. Revalidate revised patches.

Optional exploratory comparisons run `replay`, `continue`, `verdict`, then
regenerate `review` to include the new evidence. Alternating pair order reduces
one ordering bias; one pair and fixed percentage thresholds do not establish
statistical confidence. Broader performance claims require independently
reviewed held-out tasks and repeated trials using the existing eval harness.
Repository scenarios visible during discovery are not held out. See
`docs/SELF_IMPROVEMENT_RESEARCH.md` in the source tree for evidence and limits.

Promotion requires a `promote` verdict and explicit authorization:
`promote --approve --review-id ID`. The ID must match the presented patch and
current evidence. `rollback` restores the prior bundle/source pointer.
Installing a promoted bundle into the user's normal µAgent is a separate action.
Do not alter budgets, state, measurements or the controller to rescue a failed
attempt. A stopped or inconclusive experiment is a valid result.
