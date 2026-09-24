# Testing

How to run µAgent's test suites, behavioral evaluation and measurement tools,
and how CI selects them. Build and style rules live in
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Hermetic suite

The default suite needs no API key or network:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Test presets exist for `debug`, `release`, `sanitize` (ASan and UBSan), `tsan`
and `coverage`. `ctest -L source` runs only the Python and source-contract
tests; `-LE source` excludes them.

| CTest name | Covers |
| --- | --- |
| `core` | C++ unit tests (`tests/unit/`, binary `uagent_tests`) |
| `integration_runtime`, `_tools`, `_ui`, `_providers`, `_mcp`, `_delegation`, `_sandbox` | end-to-end cases against the real binary with a scripted HTTP provider and PTY (`tests/integration*.py`) |
| `integration_web`, `integration_management` | native web host; only with `UAGENT_WEB=ON` |
| `web_push` | Web Push encryption; only with `UAGENT_WEB_PUSH=ON` |
| `behavior_eval` | scenario scores against the committed baseline |
| `token_audit` | request and schema sizes against the committed baseline |
| `eval_harness_self_test`, `session_metrics_self_test` | measurement tooling (label `source`) |
| `ci_changes`, `consumer_boundary`, `wire_contract` | CI path selection and source contracts (label `source`) |
| `benchmarks` | native micro-benchmarks; only with `UAGENT_BUILD_BENCHMARKS=ON` |

Run a subset of unit or integration cases with `--list`, `--test NAME`, or
`-k SUBSTRING`:

```sh
build/debug/uagent_tests --test TestActivitySessions
build/debug/uagent_tests -k Activity
python3 tests/integration.py build/debug/uagent --group runtime --list
python3 tests/integration.py build/debug/uagent --test test_plain_turn
python3 tests/integration.py build/debug/uagent -k compaction
```

Integration cases are discovered in source order, so a new top-level `test_`
function registers itself. Each case has its own deadline;
`UAGENT_TEST_TIMEOUT_SCALE` multiplies them on slow or instrumented builds.

The SSE and input-decoder fuzzers build with the `fuzz` preset. CI runs a short
smoke pass from `tests/fuzz/corpus`; a weekly workflow runs longer.

## Web tests

Run from `web/`:

```sh
npm test                 # Node unit tests
npm run test:browser     # Playwright against a native host
```

Browser tests start `tests/web_host.py` with `UAGENT_TEST_BINARY` (default
`../build/release/uagent`). Each test owns its host, temporary HOME and
project, mock provider, pairing cookie and output directory, and waits on
visible state or an API condition rather than a fixed delay. Chromium runs
every spec; WebKit runs the layout, browser and scroll specs. Playwright
retries a failed test once locally and twice in CI (`CI ? 2 : 1`), uses two
workers in CI, and keeps traces and screenshots of failures.

## Behavioral evaluation

`benchmarks/eval.py` runs declarative scenarios from
`benchmarks/scenarios/*.json`: a workspace fixture, a prompt, a scripted
provider and the checks that define a good run (answer, files read, forbidden
tools, model rounds, batch width, deduplication, cumulative request and result
size, workspace immutability). Results are compared with
`benchmarks/baselines/hermetic.json`. `benchmarks/run_trace.py` holds the
shared run timing and trace aggregation used by the eval and the audit.

```sh
python3 benchmarks/eval.py build/debug/uagent --check
python3 benchmarks/eval.py build/debug/uagent --scenario parallel_batch
python3 benchmarks/eval.py build/debug/uagent --scenario CASE --trials 5 --pass-k 3
python3 benchmarks/eval.py build/debug/uagent --update   # review the diff
python3 benchmarks/eval.py --self-test
```

- A scenario's `tier` is `regression` (gates the build) or `capability`
  (reported only; the eval suggests promoting it once it passes).
- `variants` and `variant_env` compare settings within one scenario, for
  example `read_volume` (250/500/1000-line reads) and `superseded_reads`
  (`control`/`pruned`). A `compacted` variant fails if it scores below its
  `control`.
- Reports include pass@1, pass@k, pass^k with a Wilson 95% interval, rounds,
  idle rounds, failure categories, context size, latency and usage.

The hermetic mode scripts the provider, so it measures the harness, not model
quality.

### Live runs

`--run` replays scenarios against a real route. Repeated runs need an explicit
`--scenario`, and each trial gets a fresh workspace and HOME in seeded order.

```sh
python3 benchmarks/eval.py build/release/uagent --run --model provider/model \
  --scenario CASE --trials 5 --cost-authority authority.json \
  --report /tmp/uagent-eval.json
```

Live mode refuses to start without a `--cost-authority` file
(`uagent.eval.cost-authority.v1`, validated by `benchmarks/live_authority.py`)
that gives every selected route exactly one mode:

```json
{
  "schema": "uagent.eval.cost-authority.v1",
  "routes": {
    "provider/model": {"reports_cost": true, "enforces_hard_budget": true},
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

- **Reported cost.** `--max-cost` (default `$0.10`) is one aggregate ceiling
  across all such runs.
- **Non-billable cheap.** All five limits are required and may not exceed 12
  sessions, 8 model calls, 32 tool calls, 8,192 output tokens per call and
  300 seconds per session. They are enforced in the child through
  `UAGENT_MAX_STEPS`, `UAGENT_MAX_TOOL_CALLS`, `UAGENT_MAX_TOKENS`,
  `UAGENT_MAX_TURN_SECONDS` and a subprocess deadline, with OpenRouter
  fallbacks disabled, and checked again afterwards. Unreported cost is never
  counted as `$0`.

The report records the authority file's SHA-256 and each route's mode.

## Measurement tools

`benchmarks/audit.py` measures request and schema sizes in a fresh HOME and
checks them against `benchmarks/baselines/audit.json`. `--profile`, `--host`
and `--history` add report-only local observations that never affect the gate.

`benchmarks/session_metrics.py` summarizes real sessions from their journals
without retaining prompts or tool values: cohorts by `session.ready`
provenance (`legacy` for older journals), failed-call recovery, repeats,
argument issues, activity polls and turn outcomes.

```sh
python3 benchmarks/audit.py build/debug/uagent --check
python3 benchmarks/audit.py build/debug/uagent --profile --host --history ~/.uagent/history
python3 benchmarks/session_metrics.py --since 2026-08-01
python3 benchmarks/session_metrics.py --cohort ID --json /tmp/sessions.json
```

## CI

`.github/changes.py` selects jobs for pull requests. Changes only under `docs/`
or to top-level `README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`
or `LICENSE` run the Python job alone; adding `web/` changes adds the web job.
Any other path runs every job. Pushes to `master` and `dev`, and tags, always
run everything.

| Job | Runs |
| --- | --- |
| `build-and-test` | Release builds on Linux x86_64, Linux ARM64 and macOS ARM64; `ctest -LE source`; generated-reference check; CLI-only build; packaging |
| `sanitizers` | `sanitize` preset, `ctest -LE source` |
| `thread-sanitizer` | `tsan` preset: `core`, `integration_runtime`, `integration_tools`, `integration_web` |
| `fuzzers` | SSE and input-decoder smoke runs |
| `coverage` | `core` and integration groups with a branch report |
| `python` | Ruff check and format; `ctest -L source` |
| `cpp-style` | clang-format, cpplint and clang-tidy |
| `web` | format, Node tests, bundle and notices check, push build, Playwright |
| `CI result` | fails if any required job failed or was cancelled |

Require `CI result` in branch protection. Superseded runs are cancelled.
CodeQL runs in its own workflow on pushes, pull requests and weekly.

## Guidelines

Keep tests proportional: pure helpers get focused unit tests, and externally
visible behavior gets one hermetic integration path. Do not repeat a contract
across unit, integration and live layers. Never put secrets in prompts,
fixtures, reports or failure output.
