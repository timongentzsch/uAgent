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

## Lifecycle

### 1. Nominate

The user names one recurring behavior or explicitly authorizes evidence to be
inspected. A candidate is rejected when it has no held-out task, no binary
success criterion, or no affordable experiment that could refute it.

No raw prompt, transcript, tool output, or private repository content is stored
in experiment state.

### 2. Pre-register

`init` records the decision before results exist:

- one falsifiable hypothesis, bounded to 1,000 characters;
- exact model and effort cohort;
- 1–50 trials per variant;
- aggregate cost ceiling of at most $100;
- minimum treatment success-count delta;
- allowed percentage regression for tokens, wall time, and tool failures;
- candidate-overlay digest and byte count;
- target path and exact pre-activation byte, presence, and permission-mode
  snapshot;
- exact prior value, or absence, in the selected writable config layer and its
  scope.

The candidate must be a nonempty JSON object no larger than µAgent's 64 KiB
overlay limit. State uses versioned schemas:

```text
uagent.improvement.experiment.v1
uagent.improvement.results.v1
```

The overlay schema matches the binary exactly: `append` is a nonempty string;
`replace` is a nonempty object whose keys are one or more of `## Evidence`,
`## Tools`, `## Changes`, `## Delegation`, and `## Answer`, with string values.
Unknown keys and shapes are rejected rather than accepted as silent no-ops.

A round lives under:

```text
~/.uagent/improve/rounds/<round-id>/
├── experiment.json
├── results.json
├── candidate-overlay.json
└── rollback-overlay.bin    # only when the target existed
```

Directories are mode 0700 and files are atomically replaced at mode 0600. The
JSON records contain metrics and digests, not trial prompts or session text.
The candidate and rollback files necessarily contain the experiment artifact
and its prior bytes; they are private local artifacts and are never export
evidence.

### 3. Compare

Control and treatment trials must differ only by the prompt overlay. The runner
records, per trial:

```json
{
  "variant": "control",
  "trial": 1,
  "task_id": "task-1",
  "success": true,
  "cost_usd": 0.12,
  "tokens": 3200,
  "wall_ms": 18400,
  "tool_failures": 0
}
```

It rejects model or effort drift, mismatched opaque task IDs between paired
control/treatment trials, duplicate and out-of-range trials, negative metrics,
malformed records, unsupported schemas, and aggregate spend above the
pre-registered limit. Task IDs identify pairs without storing task content.
Trials should be interleaved to reduce time and route drift.

The source checkout's live eval harness is preferred when private tasks can be
represented as sanitized scenarios; it already supports prompt overlays,
repeated trials, and hard aggregate cost enforcement. The experiment runner
does not duplicate that harness or retain its task inputs.

### 4. Review

`review` requires every declared control and treatment trial. It computes:

- control and treatment success counts;
- success-count delta;
- mean token, wall-time, and tool-failure values;
- percentage guardrail regressions;
- aggregate reported cost.

The verdict is deterministic:

| Verdict | Rule |
| --- | --- |
| `pass` | Success delta reaches the declared minimum and every guardrail passes |
| `reject` | Success worsens or any guardrail exceeds its ceiling |
| `inconclusive` | No regression, but the declared improvement is not reached |

Thresholds cannot be changed after results are recorded. A mechanical `pass`
is necessary but not sufficient: the human still decides whether the gain is
spread across tasks, worth the always-on bytes, and belongs in a prompt overlay
rather than a narrower on-demand artifact.

### 5. Activate

`activate --approve` is accepted only after a passing review. Before writing,
the runner verifies:

- candidate bytes still match the initialization digest;
- an existing target still matches the snapshotted digest;
- an absent target has not appeared;
- the target is not a symbolic link.

It atomically writes the overlay and returns a structured configuration
proposal. It does not call or emulate `uagent_configure`.

Activation is allowed only when the selected user or project config layer can
become effective. A higher-precedence CLI flag or environment variable makes
the round evaluation-only, because writing lower-precedence config would claim
activation without changing behavior.

Activation succeeds only when the human separately approves the proposed
configuration change. If configuration approval is denied, the file exists but
is inactive.

### 6. Observe

The controlled result does not prove long-term usefulness. The user tries the
change for an explicitly bounded period and reports helped, hurt, neutral, or
inconclusive. Milestone 1 asks directly rather than pretending metadata-only
journals reveal that judgement.

Promotion into `AGENTS.md`, a workspace skill, memory, or source code is a new
reviewed change, not an automatic continuation of this experiment.

### 7. Roll back

`rollback` is valid after the reviewed overlay file has been written. It first
checks that the file still matches the candidate digest; if another actor
changed it, the runner refuses destructive restoration. Otherwise it restores
the exact prior bytes and permission mode or removes a target that did not
previously exist.

It then returns the exact configuration proposal needed to restore the prior
value in the selected layer or unset the key there. Lower-precedence values may
then become effective again. The human approves that change through
`uagent_configure`. File rollback and config rollback are distinct and must both
be reported.

## Commands

```sh
runner="skills/self-improve/scripts/experiment.py"

python3 "$runner" init --id pilot \
  --hypothesis 'Treatment improves held-out task success.' \
  --overlay /tmp/candidate-overlay.json \
  --target-overlay "$HOME/.uagent/improve/active/pilot.json" \
  --model provider/model --effort high \
  --trials 5 --max-cost 5 \
  --min-success-delta 1 --max-guardrail-regression-pct 10

python3 "$runner" record --id pilot --variant control --trial 1 \
  --task-id task-1 --model provider/model --effort high --success yes \
  --cost 0.12 --tokens 3200 --wall-ms 18400 --tool-failures 0

python3 "$runner" status --id pilot
python3 "$runner" review --id pilot
python3 "$runner" activate --id pilot --approve
python3 "$runner" rollback --id pilot
```

Installed invocations use `${SKILL_DIR}/scripts/experiment.py` as documented in
the skill.

## Tested contracts

The focused test covers:

- a complete passing control/treatment lifecycle;
- mandatory explicit activation approval;
- exact restoration of a pre-existing target's bytes and permission mode;
- removal of a target that did not previously exist;
- restoration proposals for prior set and unset config values;
- cohort and paired-task drift rejection;
- invalid trial bounds and aggregate cost rejection;
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
