---
name: self-improve
description: Run one bounded personal prompt-overlay experiment: pre-register a hypothesis, compare control and treatment trials, activate only a measured winner with human approval, and preserve exact rollback.
argument-hint: [start|status ROUND|review ROUND|rollback ROUND]
requires-tools: run, uagent_info, uagent_configure
---

# One personal improvement experiment

Improve this user's agent against evidence, not impressions. This version does
one thing: test one reversible prompt overlay. It does not mine private
transcripts automatically, rewrite memory, edit its own skill, or open a PR.

`${SKILL_DIR}/scripts/experiment.py` is the deterministic authority for state,
bounds, scoring, activation files, and rollback. The model proposes and judges
the design; it does not calculate the verdict or write µAgent configuration.

## Invariants

- Raw user prompts, session text, tool output, private code, and workspace file
  contents never enter experiment state.
- One round changes one artifact and declares one hypothesis before trials.
- Control and treatment use the same model, effort, tasks, tool policy, memory
  policy, and budgets. Only the overlay differs.
- Live trials use one explicit authority mode. Reported-cost routes need a hard
  aggregate USD ceiling. An operator-declared non-billable cheap route needs
  hard session, model-call, tool-call, output-token, and wall-clock limits.
  Missing cost data alone is never `$0`, and cheapness is never inferred from a
  model name.
- The guardrail population is pre-registered. `all-trials` compares every run;
  `paired-success` compares resources only where both matched tasks succeed.
  A control-only success always rejects, a treatment-only success counts as
  capability gain without rewarding the control for failing quickly, and a
  both-fail pair contributes neither gain nor a resource comparison.
- The treatment must improve success by the pre-registered amount and keep all
  guardrails within their declared regression ceiling.
- Activation requires two approvals: `activate --approve` may write the overlay
  file, then `uagent_configure` asks the human before changing configuration.
- Rollback restores the exact bytes and permission mode that existed before the
  round. If another actor changed the target, the runner refuses to overwrite
  it.
- Prompt text changes host text only. They never change permissions, tools,
  approval, or resource limits.
- An inconclusive result is a result. Do not tune thresholds after seeing data.

## Interpret the request

- No argument or `start`: run the complete procedure below.
- `status ROUND`: run the runner's `status` command and explain it.
- `review ROUND`: run `review`; do not activate without a later explicit yes.
- `rollback ROUND`: run `rollback`, present its configuration proposal, and use
  `uagent_configure` only after the human approves that exact change.

## 1. Choose one falsifiable problem

Use a problem the user explicitly names or evidence they explicitly authorize
you to inspect. Current metadata-only journals contain no user messages, so do
not claim they reveal corrections, restatements, skill misses, or preferences.
Never send a foreign-agent transcript to another model as part of this skill.

Write down:

- observed failure;
- one overlay change expected to prevent it;
- held-out tasks that could refute the claim;
- minimum success-count improvement;
- acceptable percentage regression for tokens, wall time, and tool failures;
- trial count and either an aggregate dollar cap or explicit non-billable cheap
  limits.

Reject a candidate when the outcome depends only on taste, there is no held-out
task, or no affordable trial can distinguish it from the control.

## 2. Inspect the cohort before writing anything

Use `uagent_info` to read the active route/status and the exact
`UAGENT_PROMPT_OVERLAY` configuration, including scope and provenance. Choose
the writable config layer that will actually be effective. Record the exact
prior value in that selected layer—not merely the effective value—for rollback;
if the key is absent in that layer, record it as unset. If a higher-precedence
CLI flag or environment variable owns the setting, `uagent_configure` cannot
make the proposed value effective: limit the work to isolated
control/treatment subprocesses and stop before activation. Do not infer support,
cost reporting, non-billable status, or cheapness from a model name.

Before a live run, require a reviewed `uagent.eval.cost-authority.v1` file for
the exact route. It must choose one mode:

- `reported-cost`: `reports_cost` and `enforces_hard_budget` are both true;
- `non-billable-cheap`: an operator explicitly sets `non_billable` and `cheap`
  true and supplies every hard limit required by `benchmarks/eval.py`.

The cheap declaration is authority, not a pricing guess. Do not create it from
session logs, provider names, or a zero-valued unreported cost.

Draft a small valid overlay JSON file in an approved scratch location. Prefer
one localized `append` or `replace` entry. Do not regenerate the whole base
prompt. Read the effective prompt reference if the target section is unclear.

Present the hypothesis, overlay diff, trial count, cost cap, and refutation rule.
Wait for approval before initializing the round.

## 3. Pre-register

Use a stable, non-identifying round ID. The state root defaults to
`~/.uagent/improve`; it is private and bounded per round.

```sh
python3 "${SKILL_DIR}/scripts/experiment.py" init \
  --id ROUND \
  --hypothesis 'ONE FALSIFIABLE SENTENCE' \
  --overlay /approved/scratch/candidate-overlay.json \
  --target-overlay "$HOME/.uagent/improve/active/ROUND.json" \
  --model 'EXACT_PROVIDER/MODEL' \
  --effort 'EXACT_EFFORT' \
  --trials 5 \
  --cost-basis reported-cost \
  --max-cost 5.00 \
  --min-success-delta 1 \
  --guardrail-mode all-trials \
  --max-guardrail-regression-pct 10 \
  --config-scope user \
  --previous-setting 'EXACT_PREVIOUS_VALUE'
```

Choose the guardrail population before any trial:

- `all-trials` preserves the original behavior and compares aggregate resources
  across every control and treatment run.
- `paired-success` is appropriate when a failed task can terminate much faster
  than a successful one. Matched treatment-only successes count toward the
  success delta but are excluded from resource comparisons; control-only
  successes reject; both-success pairs are compared; both-fail pairs are
  excluded from both gain and comparison.

For an explicitly authorized non-billable cheap route, use
`--cost-basis non-billable-cheap --max-cost 0 --cost-authority
/path/to/reviewed-authority.json`. The runner validates the exact route with the
same shared policy as the eval and stores only its SHA-256 plus normalized
limits. A pre-existing reported-cost round cannot be silently converted; start
a new round so its authority basis is pre-registered.

Omit `--previous-setting` only when the key is genuinely absent from the
selected `--config-scope` layer. Do not pass a lower-precedence effective value
as though it lived in that layer. The runner copies and hashes the candidate,
snapshots an existing target byte-for-byte, and creates versioned
`experiment.json` and `results.json`. It stores no task prompts or session text.

## 4. Run control and treatment trials

Prefer a source checkout's live eval harness when the tasks can be expressed as
sanitized scenarios. It supports `--prompt-overlay`, explicit trials, and both
live authority modes. Pass the reviewed `--cost-authority` file. The eval
injects cheap-route limits into every child process and rejects a planned
session count above the declaration before the first call. Otherwise run
user-approved isolated trials, but keep task content outside experiment state.

Interleave trials rather than running all control trials first. Keep every
cohort setting fixed. For each trial, record only:

- exact same opaque task ID in control and treatment for each trial number;
- binary task success;
- provider-reported cost, or literal `0` only when the round was pre-registered
  as `non-billable-cheap` under a valid authority;
- total tokens;
- wall-clock milliseconds;
- tool-failure count.

```sh
python3 "${SKILL_DIR}/scripts/experiment.py" record \
  --id ROUND --variant control --trial 1 --task-id task-1 \
  --model 'EXACT_PROVIDER/MODEL' --effort 'EXACT_EFFORT' --success yes \
  --cost 0.12 --tokens 3200 --wall-ms 18400 --tool-failures 0

python3 "${SKILL_DIR}/scripts/experiment.py" record \
  --id ROUND --variant treatment --trial 1 --task-id task-1 \
  --model 'EXACT_PROVIDER/MODEL' --effort 'EXACT_EFFORT' --success yes \
  --cost 0.11 --tokens 3000 --wall-ms 17200 --tool-failures 0
```

For a non-billable cheap round, take `live_authority.sha256` from the eval
report and pass it as `--authority-sha256` on every `record` command. The runner
rejects a missing or different digest, so trials cannot be recorded under a
weaker authority than the one pre-registered.

The runner rejects cohort drift, mismatched control/treatment task IDs,
duplicate/out-of-range trials, and any record that crosses the aggregate cap.
For reported-cost rounds, do not record an estimated cost when the provider did
not report one. For a pre-registered non-billable cheap round, `0` records the
operator-declared billing basis; it is not presented as provider telemetry.

## 5. Review mechanically, then judge the design

```sh
python3 "${SKILL_DIR}/scripts/experiment.py" review --id ROUND
```

The deterministic verdict uses the pre-registered guardrail mode:

- `pass`: treatment reaches the success delta, no matched capability regresses,
  and every applicable guardrail passes;
- `reject`: aggregate success worsens, a paired control succeeds where treatment
  fails, or an applicable guardrail exceeds its ceiling;
- `inconclusive`: no regression, but the required improvement was not reached.

In `paired-success`, the report includes the counts for treatment-only,
control-only, both-success, and both-fail pairs, plus the number of pairs that
contributed to each resource comparison.

Do not activate `reject` or `inconclusive`. For a `pass`, answer before asking
to activate:

- Is the result spread across tasks rather than one lucky trial?
- Is always-on prompt text the smallest appropriate artifact, or should this be
  an on-demand skill/workspace instruction instead?
- What bad behavior does this guidance make easier?
- Is the measured gain worth the prompt bytes on every affected request?

Present the exact candidate and runner-produced configuration proposal. Wait
for an explicit yes.

## 6. Activate without bypassing host approval

After explicit approval:

```sh
python3 "${SKILL_DIR}/scripts/experiment.py" activate --id ROUND --approve
```

This writes only the candidate overlay file. It prints a structured
`uagent_configure` proposal and explicitly states that configuration was not
changed. Submit that exact proposal through `uagent_configure`; do not edit a
config file with `write_file`, Python, or shell. If the configuration proposal
is denied, say that the overlay file exists but is inactive.

A changed config applies according to its reported reload policy. State plainly
whether a new turn or restart is needed.

## 7. Observe and decide

The controlled result establishes evidence, not permanence. Use the treatment
for a short user-approved trial, then ask for an explicit outcome: helped,
hurt, neutral, or inconclusive. Version one does not pretend metadata-only
journals can infer that judgement.

A successful overlay is still an experiment. Promotion to `AGENTS.md`, a
workspace skill, memory, or source code is a separate reviewed change outside
this round.

## 8. Roll back

On harm, expiry, user request, or failed activation:

```sh
python3 "${SKILL_DIR}/scripts/experiment.py" rollback --id ROUND
```

The runner restores the exact previous overlay bytes and permission mode or
removes a target that did not exist before. It refuses when the active file
changed externally. Then present its rollback configuration proposal and call
`uagent_configure` only after human approval. Report separately whether file
restoration and config restoration both succeeded.

## Stop conditions

Stop and report, without improvising, when:

- there is no falsifiable hypothesis or held-out task;
- route authority proves neither reported cost with a hard USD budget nor an
  explicit non-billable cheap declaration with complete limits;
- a proposed cheap declaration exceeds the eval's global ceilings or does not
  apply to the exact route;
- the user will not approve the declared maximum spend or cheap-route limits;
- a trial cannot be scored without retaining private text;
- cohort settings drift;
- the candidate, target, snapshot, result schema, or cost bound fails validation;
- another actor changed the active overlay;
- a CLI flag or environment variable owns `UAGENT_PROMPT_OVERLAY`;
- the result is inconclusive.

Do not respond to a stopped round by expanding scope to foreign logs, automatic
memory, source changes, or upstream PR preparation.
