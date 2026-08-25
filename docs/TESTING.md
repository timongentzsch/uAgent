# Testing

The default suite is hermetic and needs no API key:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

One case can be run on its own; the suite refuses to start when a test is
defined but missing from `TEST_ORDER`, because such a case would never run:

```sh
python3 tests/integration.py build/debug/uagent --list
python3 tests/integration.py build/debug/uagent --test test_plain_turn
python3 tests/integration.py build/debug/uagent -k compaction
```

`tests/unit/` covers local policy and protocols, including activity IDs,
head/tail buffering and LRU retention, status-row assembly, atomic pre-spawn admission, default
yielding, event-driven PTY and pipe input/output, split readiness markers, exactly-once completion delivery,
200 Ctrl+B/exit races, parallel foreground handoff, PTY input/resize, non-TTY
rejection, resumed-image matching, the public-destination policy behind
`web_fetch`, and request-payload byte stability across history mutations. Provider fixtures cover structured and
proxy-wrapped error classes, one-shot context compaction, observational
background completion, bounded task delivery, and no-replay failure paths. One shared HTTP/PTY fixture drives isolated
`runtime`, `tools`, `ui`, `providers`, `mcp`, and `delegation` CTest processes.
The SSE framing fuzz target runs in CI.

## Behavioral evaluation

`benchmarks/eval.py` scores end-to-end agent behavior against declarative
scenarios in `benchmarks/scenarios/*.json`: workspace fixture, prompt, scripted
provider, and the checks that define a good run — answer content and shape,
files read, forbidden tools, model rounds, batch width, deduplication, request
bytes, and workspace immutability. Results are compared with the committed
baseline in `benchmarks/baselines/hermetic.json`, and CTest runs that gate:

```sh
python3 benchmarks/eval.py build/debug/uagent --check
python3 benchmarks/eval.py build/debug/uagent --scenario parallel_batch
python3 benchmarks/eval.py build/debug/uagent --update   # review the diff
```

The hermetic mode scripts the provider, so it measures harness behavior — the
part this repository owns — not model quality. A prompt change moves request
bytes there; whether it moves *quality* is only visible live:

```sh
python3 benchmarks/eval.py build/release/uagent --run \
  --model provider/model --report /tmp/uagent-eval.json
python3 benchmarks/eval.py build/release/uagent --run --model provider/model \
  --prompt-overlay experiment.json
```

Live runs are billable, require `--run`, and apply `--max-cost` (default
`$0.10`) to each isolated run. Providers that do not report cost cannot make a
dollar cap authoritative. A compacted run fails if its score is below its
control, hermetically and live.

Scenarios carry a `tier`. A `capability` scenario is reported but does not gate
the build — it is a hill to climb — and graduates to `regression` once it holds
green. The suite reports rounds that asked for nothing (`idle`) and the
failure-category vector alongside the score, because a pass rate alone does not
say what broke.

## Improvement iterations

`benchmarks/session_metrics.py` reports what real sessions did and where they
spent time, tokens and turns; `benchmarks/audit.py` prints the six-clause
dashboard — hardware, token, speed, capability, readability, and whether the
scenario suite still resembles real usage — and fails on a baseline regression:

```sh
python3 benchmarks/session_metrics.py --since 2026-08-01
python3 benchmarks/audit.py build/debug/uagent --check
python3 benchmarks/audit.py build/debug/uagent --update   # review the diff
```

The audit reads session journals and a configured build tree, so it is a local
tool rather than a CI gate. The `self-improve` skill drives the whole loop.
`--prompt-overlay` is what makes a before/after cohort comparable without
rebuilding.

Keep tests proportional: pure helpers get focused unit coverage; externally
visible behavior gets one hermetic integration path. Avoid duplicating the
same contract across unit, integration, simulation, and live-model layers.
Never put secrets in prompts, fixtures, reports, or failures.
