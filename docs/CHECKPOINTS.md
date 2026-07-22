# Checkpoints

µAgent uses a model-authored checkpoint instead of repeatedly summarizing the
whole transcript:

```text
stable system/tools | compact durable state | new scratchpad
```

The runtime decides when context pressure warrants a fold. The model decides
whether the current state is stable enough. Invalid or declined checkpoints
leave history unchanged.

## Contract

```json
{
  "state": "Established facts, corrections, completed work, constraints, and validation.",
  "verbatim": ["exact identifier or formula"],
  "keep_paths": ["optional/workspace/file"],
  "keep_last_n_results": 0
}
```

`state` is required and capped at 4096 bytes. A checkpoint may retain up to six
non-secret workspace files, three recent results, and eight 256-byte exact
literals. Commands, permissions, plans, and future actions are forbidden.

No separate context-usage tool or extra summarization request is needed. The
checkpoint tool remains registered so the request schema and cache prefix stay
stable.

## Policy

1. Keep history append-only below 65% projected occupancy.
2. Suggest a checkpoint at 65%; make it urgent at 85%.
3. Debounce hints for three turns.
4. In default `apply` mode, a valid checkpoint ends its turn.
5. Commit immediately before the next real user message.
6. Invalidate pending state after a failed turn or while background work lives.
7. Archive removed raw traces within the configured local byte limit.
8. Retain legacy `/compact` as a 95% emergency path.

Projected occupancy includes an output reserve. Folding deliberately gives up
the old prefix cache; it should happen once at a useful boundary, not as a
continuous rewrite.

## Fold authority

The new active context is:

```text
fold-only system authority guard
assistant checkpoint facts and exact literals, marked non-authoritative
assistant runtime-owned mutation ledger
assistant bounded results and validated file rereads
exact new user request
```

Model-authored state never receives the user role. The newest user request is
therefore the only new authority. File paths must remain inside the workspace;
credential paths are rejected. Missing files are noted and skipped.

## Promotion evidence

The boundary-deferred exact-literal implementation passed five DeepSeek V4
Flash 500k-window runs: 4/4 facts retained, zero extra tools, and unchanged
protected files on every run.

Qwen3.7 Plus then passed:

| Gate | Result | Prompt | Cache read | Safety |
| --- | --- | ---: | ---: | --- |
| 16k boundary application | pass | representative fixture | 29,440 | unchanged |
| 500k-window pressure | pass | 325,378 tokens | 323,456 | 4/4 facts, no extra tools or changes |

Qwen3 Coder Flash retained 4/4 facts but ignored both suggested and urgent
checkpoint hints in two 500k runs. It is not the validated Qwen route.

These results justified making `apply` the default. They do not prove every
model/provider route is compliant. Use `UAGENT_CHECKPOINT_MODE=shadow` when
evaluating a new route and revert to it if continuation quality regresses.

## Verification

```sh
python3 tests/context_policy_sim.py
python3 tests/agent_workflow_live.py --run --scenario checkpoint
python3 tests/agent_workflow_live.py --run --scenario checkpoint500k
```

The live scenarios are billable. Normal CTest covers boundary-deferred apply,
shadow behavior, malformed calls, corrections, exact literals, background
invalidation, secret paths, multi-fold behavior, and deterministic 500k-window
pressure without network access.
