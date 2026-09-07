# Self-improvement instruction

This file is the fixed instruction `I` of the recursive self-improvement loop.
Every executor in a generation receives exactly this text, so it is versioned
here, hashed into the generation manifest, and treated as a protected input
that a candidate may not rewrite mid-experiment.

---

Read `docs/ARCHITECTURE.md` and verify the relevant paths in the current source.
Identify and implement one architectural improvement to the harness that
benefits distinct development workflows, without weakening existing behavior,
tests, gates or constraints. Target a shared mechanism: for example context
construction, tool execution, state ownership, recovery, or duplicated runtime
paths. Optimize for the reach and usefulness of the change, not patch size.

Trace the relevant execution paths and identify a recurring bottleneck or
structural limitation before choosing a change. Compare promising options by
expected impact, supporting evidence, complexity, and implementation and
verification cost. Prefer simplifying or reusing a shared mechanism over
adding a new abstraction. A small patch can qualify when its architectural
effect is broad; a larger patch or a shared helper alone does not establish it.
Presentation tweaks and isolated edge-case fixes are incidental findings,
not successful outcomes for this skill. Do not select one just because its
fail-to-pass check is easy to produce.

Work only inside the workspace you were given. It is a disposable copy of the
source; the checkout it came from, the controller that launched you and the
results of this experiment are outside your reach by design.

Before you make the substantive edit, decide and write down:

1. the shared limitation, the architectural change, and a falsifiable prediction
   about its effect;
2. the distinct workflows that exercise that mechanism, with before/after
   checks and the command that runs them;
3. what would refute the claimed benefit, including regressions or complexity
   that outweighs the gain.

Implement the smallest coherent architectural change, update its consumers,
and remove the superseded path where appropriate. Verify the affected workflows
and pre-existing gates. Record results per workflow rather than treating
several inputs to one edge-case test as evidence of broad impact. For structural
simplification, show which duplicated paths or ownership rules were eliminated
and that their consumers preserve behavior; do not invent a speedup. Keep
generated logs and caches out of the proposed source patch. Stop after one
bounded architectural improvement attempt.

After verification, complete the claim's `assessment`: summarize the change,
observed impact, expected generality and why, limitations and untested cases,
your recommendation (`propose`, `revise` or `reject`), and a proposed commit/PR
title. In `generality`, connect the changed mechanism to the distinct workflows
tested, their observed results, and the expected but untested reach. Distinguish
measured facts from predictions. Passing one local regression is insufficient
evidence for an architectural recommendation.
Cite primary sources for external research claims and recorded commands/results
for local empirical claims. State uncertainty when evidence is missing.
Do not commit, open a PR, install or activate the candidate. The operator
presents your assessment, the exact diff and controller evidence to the human
before requesting authorization for those actions.

If no architectural candidate can be justified and verified within the frozen
budget, report `no validated architectural improvement`, the strongest proposal
and the evidence or budget it still needs. Do not substitute a micro fix or
increase the budget mid-run. A cosmetic edit, a reformatting pass, a weakened
test, a deleted gate or an unverified claim is
worse than that outcome, and the controller rejects it.
