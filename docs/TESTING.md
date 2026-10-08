# Testing

How to run µAgent's test suites, behavioral evaluation and measurement tools,
and how CI selects them. Build and style rules live in
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Common cases

The suite needs no API key or network. Build the debug tree while iterating:
the release tree links with LTO, about 20 s after any edit.

```sh
cmake --preset debug
cmake --build --preset debug --parallel
```

| To check | Run | About |
| --- | --- | --- |
| one unit test area | `build/debug/uagent_tests -k Activity` | seconds |
| one unit test | `build/debug/uagent_tests --test TestActivitySessions` | seconds |
| one integration test | `python3 tests/integration.py build/debug/uagent --test test_plain_turn` | seconds |
| integration tests by name | `python3 tests/integration.py build/debug/uagent -k compaction` | seconds |
| a web edit | `npm test` and `npm run typecheck` in `web/` | 2 s |
| one web spec in a browser | `npm run build`, then `npm run test:spec -- tests/ui.spec.js` in `web/` | 10 s + the spec |
| everything before a commit | `ctest --preset debug` | about 50 s |
| web work before a push | `npm run test:browser` in `web/` (both browsers) | minutes |
| what only CI's compilers and linters report | `.github/ci-local.sh` | minutes; incremental after the first run |

Style checks (clang-format, cpplint, clang-tidy, Ruff, Prettier) are in
[CONTRIBUTING.md](../CONTRIBUTING.md). [CI](#ci) lists what a pull request
runs beyond this.

## Hermetic suite

```sh
ctest --preset debug                      # everything, six tests at a time
ctest --preset debug -R '^core$'          # one CTest by name
ctest --preset debug -L source            # only the source contracts
ctest --preset debug -LE source           # everything else
```

Test presets exist for `debug`, `release`, `sanitize`, `tsan` and `coverage`.

| CTest name | Covers |
| --- | --- |
| `core` | C++ unit tests (`tests/unit/`, binary `uagent_tests`) |
| `integration_runtime`, `_tools`, `_ui`, `_providers`, `_mcp`, `_delegation`, `_sandbox` | end-to-end cases against the real binary with a scripted HTTP provider and PTY (`tests/integration*.py`) |
| `integration_web`, `integration_management` | native web host; only with `UAGENT_WEB=ON` |
| `web_push` | Web Push encryption; only with `UAGENT_WEB_PUSH=ON` |
| `behavior_eval` | scenario scores against the committed baseline |
| `eval_harness_self_test`, `session_metrics_self_test` | measurement tooling (label `source`) |
| `ci_changes`, `layer_boundary`, `wire_contract` | CI path selection and source contracts (label `source`) |
| `benchmarks` | native micro-benchmarks; only with `UAGENT_BUILD_BENCHMARKS=ON` |

### Selecting cases

Both runners take `--list`, `--test NAME` (exact) and `-k SUBSTRING`. The
integration runner also takes `--group` and `-j`, and repeats `--test` and
`-k`:

```sh
build/debug/uagent_tests --list
python3 tests/integration.py build/debug/uagent --group runtime --list
python3 tests/integration.py build/debug/uagent --group tools -j 8
python3 tests/integration.py build/debug/uagent -j 8      # every group
```

The groups are `runtime`, `tools`, `ui`, `providers`, `mcp`, `delegation`,
`sandbox`, `web` and `management`; `--list` prints each case with its group.

- A new top-level `test_` function in a group's module registers itself.
- Each case has its own deadline. `UAGENT_TEST_TIMEOUT_SCALE` multiplies
  them on slow or instrumented builds.
- A failing case does not stop the run: every case runs, and the failures
  are listed at the end.
- `-j N` runs the selection in N processes. Every case has a home and ports
  of its own, so the whole suite takes about 20 s at `-j 8` against two
  minutes in one.

### Sanitizers, coverage and fuzzers

```sh
cmake --preset sanitize && cmake --build --preset sanitize --parallel
ctest --preset sanitize -LE source        # ASan and UBSan

cmake --preset tsan && cmake --build --preset tsan --parallel
ctest --preset tsan -R '^(core|integration_(runtime|tools|web))$'

cmake --preset coverage && cmake --build --preset coverage --parallel
ctest --preset coverage -R '^(core|integration_.*)$'
```

Runtimes have no stderr, so each process writes its sanitizer report to
`build/sanitize/report.*` or `build/tsan/report.*`. Instrumented runs need
longer deadlines; CI sets `UAGENT_TEST_TIMEOUT_SCALE=6` for both sanitizer
jobs.

The SSE and input-decoder fuzzers need Clang:

```sh
CC=clang CXX=clang++ cmake --preset fuzz
cmake --build --preset fuzz --parallel
cp -r tests/fuzz/corpus /tmp/corpus      # libFuzzer writes into its corpus
build/fuzz/uagent_fuzz_sse -runs=1000 -max_len=8192 /tmp/corpus/sse
build/fuzz/uagent_fuzz_input_decoder -runs=2000 -max_len=4096 \
  /tmp/corpus/input_decoder
```

That is CI's smoke pass. A weekly workflow (`fuzz.yml`) searches for five
minutes per fuzzer and uploads the corpus it reached.

## Web tests

Run from `web/`:

```sh
npm test                 # Node unit tests (tests/*.test.js)
npm run typecheck
npm run test:browser     # Playwright against a native host, both browsers
npm run test:spec -- tests/ui.spec.js   # one spec: Chromium, no retry, stops at a failure
```

Which binary and bundle a browser test drives:

- The host is `tests/web_host.py`, started with `UAGENT_TEST_BINARY`.
  Without it: `../build/debug/uagent` locally, `../build/release/uagent` in
  CI and for `performance.spec.js`, whose timings are a release build's.
- Locally the host serves the bundle in `web/dist` from disk, so a web edit
  needs `npm run build` and no rebuild of the binary. In CI it serves the
  bundle embedded in the binary, as a release does.

How the tests are isolated:

- Each test owns its host, temporary HOME and project, mock provider, pairing
  cookie and output directory. It waits on visible state or an API condition
  rather than a fixed delay.
- The showcase's dev server takes a port derived from the checkout's path and
  is never reused if one already listens there, so a run cannot test another
  checkout's tree.

What a spec may assume about time:

- The composer sends and attaches nothing until the event stream has caught
  up. A Playwright click waits for its control to enable; `press("Enter")`,
  `setInputFiles` and a click scripted inside the page do not, and what they
  carried is lost without an error. `await online(page)` (`fixtures.js`)
  comes before the first of them, and again after a step that reconnects.
- A layer closed from the interface (a popover, a dialog) removes its history
  entry in a step back of its own. A spec that then navigates by
  `location.hash` first waits for `history.state?.layer` to be gone.
- Speed is recorded, not asserted: frame gaps, input latency and open times
  go into attachments (`web-performance.json`, `reload-frame-gaps.json`) and
  never fail a run.
- The only fixed pauses are gestures: the long-press holds in
  `browser.spec.js` and `showcase.spec.js`, and the pauses between the wheel
  and scroll steps in `scroll-stick.spec.js`.

Playwright has two projects. `chromium` runs every spec. `webkit` runs `ui`,
`ui-quality`, `browser`, `showcase`, `dismiss`, `history-anchor`,
`scroll-restore`, `scroll-stick` and `coordinator`. A failed test is retried
once, locally and in CI; four workers run locally and two in CI; traces
and screenshots of failures are kept in `web/test-results`.

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
  example `read_volume` (250/500/1000-line reads). A `compacted` variant fails
  if it scores below its `control`.
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
  `UAGENT_MAX_TURN_SECONDS` and a subprocess deadline, and checked again
  afterwards. Unreported cost is never counted as `$0`.

The report records the authority file's SHA-256 and each route's mode.

## Measurement tools

`benchmarks/audit.py` reports request and schema sizes from a fresh HOME;
`behavior_eval` gates their growth per scenario. `--profile`, `--host` and
`--history` add local observations.

`benchmarks/session_metrics.py` summarizes real sessions from their journals
without retaining prompts or tool values: cohorts by `session.ready`
provenance (`legacy` for older journals), failed-call recovery, repeats,
argument issues, activity polls and turn outcomes.

```sh
python3 benchmarks/audit.py build/debug/uagent
python3 benchmarks/audit.py build/debug/uagent --profile --host --history ~/.uagent/history
python3 benchmarks/session_metrics.py --since 2026-08-01
python3 benchmarks/session_metrics.py --cohort ID --json /tmp/sessions.json
```

## CI

`.github/workflows/ci.yml` runs on pull requests, pushes to `master`, `v*`
tags and weekly.

On a pull request, `.github/changes.py` selects jobs by the paths changed:

| Changed paths | Jobs |
| --- | --- |
| only `docs/`, `README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, `LICENSE` | `python` |
| those and `web/` | `python`, `web` |
| anything else | every job |

A pull request that touches native code runs what a push to `master` runs,
with two exceptions that `master` then covers. `coverage` runs on `master`
only. `clang-tidy` reads the translation units the pull request changed, in
one job; when it changes a header, the build files or the rules, it reads
all of them, as `master` always does. A finding in an unchanged unit that
only `master` reports is possible for a change to generated or vendored
code, not for a header.

The Linux build jobs reuse compiled objects through ccache
(`.github/actions/ccache`): restored in every run, saved only from `master`.
A newer push to a pull request cancels the run of the older one; runs on
`master` are never cancelled, so each merge is tested as it landed. A browser
spec gets one retry, locally and in CI.

| Job | Runs |
| --- | --- |
| `web-dist` | The web bundle the native builds embed; skipped when only `web/` or documents changed |
| `build-and-test` | Release builds with Web Push on Linux x86_64, Linux ARM64 and macOS ARM64; `ctest -LE source`; generated-reference check (Linux x86_64); packaging |
| `variants` | CLI-only and no-browser Release builds with their tests |
| `sanitizers` | `sanitize` preset, `ctest -LE source` |
| `thread-sanitizer` | `tsan` preset: `core`, `integration_runtime`, `integration_tools`, `integration_web` |
| `fuzzers` | SSE and input-decoder smoke runs |
| `coverage` | `core` and every integration group with a branch report; fails under 67% line coverage |
| `python` | Ruff check and format; `ctest -L source` |
| `cpp-style` | clang-format and cpplint |
| `clang-tidy` | clang-tidy, translation units split across three runners |
| `web` | Prettier check, Node tests, bundle, notices and size checks, a release host with Web Push, Playwright in Chromium and WebKit |
| `CI result` | fails if any required job failed or was cancelled |

The `master requires CI` ruleset requires `CI result`, so a red run blocks a
merge. Superseded runs are cancelled. GitHub sends two events for a pull
request stacked on another's branch when both are pushed together: the first
run is then cancelled within seconds, and its `CI result` reads failed.
CodeQL runs in its own workflow on pushes, pull requests and weekly.

## Guidelines

Keep tests proportional: pure helpers get focused unit tests, and externally
visible behavior gets one hermetic integration path. Do not repeat a contract
across unit, integration and live layers. Never put secrets in prompts,
fixtures, reports or failure output.

A test passes or fails the same way on a slow machine:

- Wait for the event (a reply row, a state, a file), never for a duration,
  and not for the echo of what the test itself typed.
- A deadline only bounds a wait. Use the shared ones (`budget()` in
  `tests/integration_support.py`; `timeout` and `expect.timeout` in
  `web/playwright.config.js`) and name a longer one only where a step is
  known to be long.
- A command that stands for "still running" outlasts the test (`sleep 60`),
  and the test stops it.
- The runtime's own request and stream timeouts are hang guards in tests. A
  case about a timeout sets it itself; no other case may depend on one
  firing.
- After a look at state, act on it only if a refusal is handled: the state
  may have moved on.
- Text expected in the terminal's working row fits it at any elapsed time:
  the row clips its label to the columns its counters leave.
