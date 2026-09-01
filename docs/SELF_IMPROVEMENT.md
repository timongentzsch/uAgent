# Self-improvement

Status: Milestone 1 implemented on August 28, 2026.

µAgent's first self-improvement feature is deliberately narrow: one controlled,
reversible prompt-overlay experiment for one user-specific problem. It does not
claim to infer preferences from metadata-only journals, rewrite its own memory,
or autonomously contribute code upstream.

The product hypothesis is:

> A user can name one recurring failure, µAgent can pre-register one bounded
> instruction change, compare it with a control on held-out tasks, activate a
> measured winner with human approval, and restore the exact prior state.

If this loop cannot produce a noticeable improvement, broader telemetry and
cross-agent log ingestion would add machinery without validating the premise.

## Why the scope changed

The earlier design began with six generic friction detectors. Inspection of 81
local journals on August 28, 2026 showed that the source cannot support them:

- journals contain metadata, not user messages or prompt bodies;
- ordinary calls often have no argument digest;
- only six of those journals contained `session.ended`;
- correction, restatement, selected-skill, edit-revert, and semantic memory
  churn cannot be derived honestly from the recorded fields.

`benchmarks/session_metrics.py` can report folded tool-result failures, but it
gets that text from the sensitive transcript beside the journal, not from the
journal itself. Milestone 1 therefore does not mine transcripts or invent weak
proxies. It tests the causal loop directly with an explicit user-nominated
problem.

## Components

```text
skills/self-improve/
├── SKILL.md
└── scripts/
    └── experiment.py

tests/
└── self_improve_experiment_test.py
```

The runner is inside the bundled skill because `benchmarks/` is a source-tree
facility and is not installed. CMake packages the complete skill tree.

| Component | Authority |
| --- | --- |
| Model/skill | Form the hypothesis, draft the overlay, choose held-out tasks, explain trade-offs |
| Runner | Validate schemas and bounds, store aggregate results, calculate the verdict, write activation files, restore snapshots |
| `uagent_configure` | Apply or restore `UAGENT_PROMPT_OVERLAY` after mandatory human approval |
| Human | Approve spend, initialization, activation, configuration, and the final usefulness judgement |

The runner never writes µAgent configuration. `activate` writes the reviewed
overlay file and returns an exact `uagent_configure` proposal. Configuration
and file activation remain separate outcomes and are reported separately.

## Operational procedure

The release-matched procedure and command syntax live in
`skills/self-improve/SKILL.md`; its installed `${SKILL_DIR}` paths work outside
a source checkout. This document keeps the design, authority boundaries, tested
contracts, and milestones rather than duplicating the executable runbook.

## Tested contracts

The focused test covers:

- a complete passing control/treatment lifecycle;
- mandatory explicit activation approval;
- exact restoration of a pre-existing target's bytes and permission mode;
- removal of a target that did not previously exist;
- restoration proposals for prior set and unset config values;
- all five paired-success branches: treatment-only gain, control-only
  regression, both-success within and above the ceiling, and both-fail;
- cohort and paired-task drift rejection;
- invalid trial bounds, reported-cost caps, and explicit zero-cost cheap-mode
  enforcement;
- unsupported schemas, unknown state fields, and oversized state-file rejection;
- refusal to follow a symbolic-link target or overwrite externally changed
  content or permission mode.

The test is registered as `self_improve_experiment` in CTest. Release package
validation already compares every file in `skills/` with the packaged skill
tree, so the runner cannot silently disappear from archives.

## Explicit non-goals

Milestone 1 does not add:

- automatic correction or restatement detectors;
- Claude Code, Codex, or opencode transcript adapters;
- a durable itemized memory format;
- unattended activation;
- automatic source edits;
- branch creation or PR submission;
- claims that local recurrence proves generality or anonymity.

## Gates for later milestones

Broader work begins only after at least one real experiment:

1. improves held-out tasks under the declared guardrails;
2. survives temporary use and is judged helpful by the user;
3. rolls back mechanically;
4. reveals a concrete telemetry gap that would have changed the decision.

Only then should µAgent add the smallest metadata event needed for that gap.
Foreign-log ingestion is considered only if candidate discovery—not causal
evaluation—is the demonstrated bottleneck. Upstream contribution is considered
only for a finding reproducible from public or synthetic evidence on a clean
branch from `origin/main`; private transcript-derived text is never PR evidence.
