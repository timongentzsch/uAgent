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

`benchmarks/session_metrics.py` reports what real sessions did and where they
spent time, tokens and turns. It cohorts canonical, allowlisted
`session.ready` provenance (`legacy` is explicit), supports `--cohort`, and
derives failed-call recovery, identical repeats, argument issues, quiet/terminal
activity polls and turn outcomes offline. `benchmarks/audit.py` prints the
dashboard — hardware, token, speed, capability, readability, and whether the
scenario suite still resembles real usage — and fails on a baseline regression:

```sh
python3 benchmarks/session_metrics.py --since 2026-08-01
python3 benchmarks/session_metrics.py --cohort legacy
python3 benchmarks/session_metrics.py --self-test
python3 benchmarks/audit.py build/debug/uagent --check
python3 benchmarks/audit.py build/debug/uagent --update   # review the diff
python3 benchmarks/slopscan.py --verbose
python3 benchmarks/slopscan.py --self-test   # check the checks
```

`slopscan.py` scans the tree, not only the latest diff, for unreachable code,
unused declarations, duplicated blocks, stale file references, and duplicated
documentation. Its low-noise heuristics are baselined in
`benchmarks/baselines/slop.json`; exceeding a baseline exits nonzero. Review
findings, not just counts.

Because a broken scanner could also report zero, `--self-test` checks
`tests/fixtures/slop`, which contains one intentional instance of each defect.
CI runs the self-test before the scan.

The audit reads session journals and a configured build tree, so it stays a
local tool. The `self-improve` skill drives the whole loop.
`--prompt-overlay` is what makes a before/after cohort comparable without
rebuilding.

Keep tests proportional: pure helpers get focused unit coverage; externally
visible behavior gets one hermetic integration path. Avoid duplicating the
same contract across unit, integration, simulation, and live-model layers.
Never put secrets in prompts, fixtures, reports, or failures.
