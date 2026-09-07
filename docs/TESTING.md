# Testing

The default suite is hermetic and needs no API key:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -j6 --output-on-failure
```

Run one C++ case with `--test` or filter cases with `-k`:

```sh
build/debug/uagent_tests --list
build/debug/uagent_tests --test TestActivitySessions
build/debug/uagent_tests -k Activity
```

Run one integration case with `--test` or filter cases with `-k`. Integration
cases are discovered from each module in source order, so adding a top-level
`test_` function registers it automatically:

```sh
python3 tests/integration.py build/debug/uagent --list
python3 tests/integration.py build/debug/uagent --test test_plain_turn
python3 tests/integration.py build/debug/uagent -k compaction
```

`tests/unit/` covers local policy and protocols: activity IDs, bounded output
and retention, admission, yielding, PTY/pipe I/O, readiness markers,
exactly-once completion, Ctrl+B/exit races, foreground handoff, non-TTY
rejection, resumed images, public-destination policy, and request-payload
stability. Provider fixtures cover errors, context compaction, observational
background completion, bounded task delivery, and no-replay failures. One
shared HTTP/PTY fixture drives isolated `runtime`, `tools`, `ui`, `providers`,
`mcp`, and `delegation` CTest processes. CI runs the SSE framing fuzz target.

## Behavioral evaluation

`benchmarks/eval.py` scores end-to-end agent behavior against declarative
scenarios in `benchmarks/scenarios/*.json`: workspace fixture, prompt, scripted
provider, and the checks that define a good run — answer content and shape,
files read, forbidden tools, model rounds, batch width, deduplication, cumulative
request/tool-result characters, active-schema recovery, and workspace
immutability. Results are compared with the committed baseline in
`benchmarks/baselines/hermetic.json`, and CTest runs that gate:

```sh
python3 benchmarks/eval.py build/debug/uagent --check
python3 benchmarks/eval.py build/debug/uagent --scenario parallel_batch
python3 benchmarks/eval.py build/debug/uagent --scenario CASE --trials 5 --pass-k 3
python3 benchmarks/eval.py --self-test
python3 benchmarks/eval.py build/debug/uagent --update   # review the diff
```

The hermetic mode scripts the provider, so it measures harness behavior — the
part this repository owns — not model quality. A prompt change moves request
bytes there; whether it moves *quality* is only visible live:

```sh
python3 benchmarks/eval.py build/release/uagent --run \
  --model provider/model --scenario browser_outcome_rounds --trials 5 \
  --cost-authority /path/to/authority.json --report /tmp/uagent-eval.json
python3 benchmarks/eval.py build/release/uagent --run --model provider/model \
  --scenario CASE --prompt-overlay experiment.json \
  --cost-authority /path/to/authority.json
```

Live runs make real provider calls and require `--run`; they may be billable or
explicitly operator-declared non-billable. Repeated runs require an explicit
scenario and get a fresh workspace and HOME in deterministic seeded order. The
report groups route/model/provenance cohorts and includes pass@1, pass@k,
pass^k, a Wilson 95% interval, rounds, cumulative context, result characters,
latency and normalized usage.

`--max-cost` (default `$0.10`) is one aggregate ceiling for routes that
report cost, not a per-run allowance. Before making any call, live mode requires
a `--cost-authority` JSON file with schema `uagent.eval.cost-authority.v1`.
Each selected route chooses exactly one authority mode.

A normal billable route must explicitly report costs and enforce the hard USD
budget:

```json
{
  "schema": "uagent.eval.cost-authority.v1",
  "routes": {
    "provider/model": {"reports_cost": true, "enforces_hard_budget": true}
  }
}
```

An operator may instead declare an exact route both non-billable and cheap.
This is an explicit policy statement, never a model-name heuristic. All five
limits are mandatory:

```json
{
  "schema": "uagent.eval.cost-authority.v1",
  "routes": {
    "local/cheap-model": {
      "non_billable": true,
      "cheap": true,
      "limits": {
        "max_sessions": 6,
        "max_model_calls": 3,
        "max_tool_calls": 8,
        "max_output_tokens_per_call": 4096,
        "max_session_seconds": 120
      }
    }
  }
}
```

The eval rejects declarations above global cheap-mode ceilings: 12 sessions,
8 model calls per session, 32 tool calls per session, 8192 output tokens per
model call, and 300 seconds per session. It enforces those limits in the child
process through `UAGENT_MAX_STEPS`, `UAGENT_MAX_TOOL_CALLS`,
`UAGENT_MAX_TOKENS`, `UAGENT_MAX_TURN_SECONDS`, request/stream deadlines, and a
matching subprocess deadline. OpenRouter fallback is disabled for cheap-authority
children so the exact attested route cannot silently escape to a different
billing path. The planned session count is rejected before the first call.
Results are checked against the same limits afterward.

The self-improvement controller additionally accepts `max_model_calls: 0` to
disable that cap while retaining its total-token and other run limits. This
exception does not apply to general-purpose live eval.

A route without one complete declaration is blocked rather than tried
optimistically. `--max-cost` applies only to reported-cost routes; a
non-billable declaration does not turn unavailable provider cost into a
reported `$0`. The report records `live_authority.sha256` and each route's
normalized authority mode so downstream experiment records can bind themselves
to the exact reviewed file. A compacted run fails if its score is below its
control, hermetically and live.

Scenarios carry a `tier`. A `capability` scenario is reported but does not gate
the build — it is a hill to climb — and graduates to `regression` once it holds
green. The suite reports rounds that asked for nothing (`idle`) and the
failure-category vector alongside the score, because a pass rate alone does not
say what broke.

## Improvement iterations

The `read_volume` scenario compares 250/500/1000-line defaults, including the
extra requests needed to continue reading. `superseded_reads` compares control
with opt-in step-boundary pruning. Both use `variant_env` to apply only the
settings under test:

```sh
python3 benchmarks/eval.py build/release/uagent --scenario read_volume \
  --scenario superseded_reads --trials 3 --report /tmp/efficiency.json
```

These scripted trials verify mechanisms and cumulative accounting, not model
quality. Use the live authority procedure above before changing defaults. The
native benchmark also compares full and cached payload preparation for every
wire API; these timings exclude network and generation latency.

`benchmarks/session_metrics.py` reports what real sessions did and where they
spent time, tokens and turns. It cohorts canonical, allowlisted
`session.ready` provenance (`legacy` is explicit), supports `--cohort`, and
derives failed-call recovery, identical repeats, argument issues, quiet/terminal
activity polls and turn outcomes offline. `benchmarks/audit.py` prints the
request/schema report and checks deterministic measurements against its baseline:

```sh
python3 benchmarks/session_metrics.py --since 2026-08-01
python3 benchmarks/session_metrics.py --self-test
python3 benchmarks/audit.py build/debug/uagent --check
python3 benchmarks/audit.py build/debug/uagent --profile --host --history ~/.uagent/history
```

The check uses a fresh HOME, validated request telemetry and explicit source
contracts. Missing telemetry or a baseline is an error. Personal profile,
history and build observations are opt-in reports and cannot affect the gate.
Ruff runs once in CI over tests, benchmarks and shipped skill scripts. Regex
reachability, prose/style and clone-count gates have been removed; compiler
warnings, clang-tidy, package/discovery and generated-reference checks remain.
Python/source-only tests carry the `source` CTest label and run in one CI job;
`ctest --preset debug` still runs the complete local suite. Native and sandbox
controller tests retain the platform matrix. Both runners report case times.

Self-improvement is measured separately and does not use overlays:
`tests/self_improve_controller_test.py` (CTest `self_improve_controller`) drives
baseline preflight, discovery, gate, human review artifacts, paired replay,
continuation, verdict, review-bound promotion and rollback against a scripted
stand-in binary. It checks failure before model calls, patch applicability and
approval binding without a model call. This verifies controller behavior, not
live-model improvement or generalization. `benchmarks/eval.py` and
`benchmarks/audit.py` share that loop's run, metric and authority primitives
from `skills/self-improve/scripts/`, so a change there is exercised by the eval
self-test as well. See `docs/SELF_IMPROVEMENT.md`.

Keep tests proportional: pure helpers get focused unit coverage; externally
visible behavior gets one hermetic integration path. Avoid duplicating the
same contract across unit, integration, simulation, and live-model layers.
Never put secrets in prompts, fixtures, reports, or failures.
