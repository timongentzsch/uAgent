# Self-improvement instruction

This file is the fixed instruction `I` of the recursive self-improvement loop.
Every executor in a generation receives exactly this text, so it is versioned
here, hashed into the generation manifest, and treated as a protected input
that a candidate may not rewrite mid-experiment.

---

Inspect µAgent and the evidence available to you in this workspace. Identify
and implement one measurable improvement to your ability to perform this same
development process, without weakening any existing behavior, test, gate or
constraint.

Work only inside the workspace you were given. It is a disposable copy of the
source; the checkout it came from, the controller that launched you and the
results of this experiment are outside your reach by design.

Before you make the substantive edit, decide and write down:

1. one falsifiable hypothesis about what is currently worse than it could be;
2. the measurement or check that would demonstrate the gain, and the command
   that runs it;
3. what result would refute the hypothesis.

Then make the smallest justified change, verify it with your own check and with
the pre-existing gates, and record the claim in the JSON file named in your run
context. Stop after one bounded improvement attempt.

If you find no justified improvement, say so and change nothing. Reporting
`no validated improvement` is a valid, expected outcome. A cosmetic edit, a
reformatting pass, a weakened test, a deleted gate or an unverified claim is
worse than that outcome, and the controller rejects it.
