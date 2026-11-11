# Testing

How to run the test suites, the behavioral evaluation and the measurement
tools, and how CI selects them. Build and style rules are in
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Common cases

The suite needs no API key or network. Iterate on the debug tree; the release
tree links with LTO, which makes incremental linking slower. Build the web
bundle before configuring or rebuilding the native binary so embedded-asset
checks see the current bundle. Run these steps sequentially:

```sh
(cd web && npm ci && npm run build)
cmake --preset debug
cmake --build --preset debug --parallel
```

| To check | Run | About |
| --- | --- | --- |
| one unit test area | `build/debug/uagent_tests -k Activity` | seconds |
| one unit test | `build/debug/uagent_tests --test TestActivitySessions` | seconds |
| one integration test | `python3 tests/integration.py build/debug/uagent --test test_plain_turn` | seconds |
| integration tests by name | `python3 tests/integration.py build/debug/uagent -k compaction` | seconds |
| a web edit | `npm test` and `npm run typecheck` in `web/` | seconds |
| one web spec in a browser | `npm run build`, then `npm run test:spec -- tests/phone-layout.spec.js` in `web/` | build + the spec |
| the native and integration suites | `ctest --preset debug` | minutes |
| web work before a push | `npm run test:browser` in `web/` (both browsers) | minutes |
| what only CI's compilers and linters report | `.github/ci-local.sh` | minutes; incremental after the first run |

Style checks (clang-format, cpplint, clang-tidy, Ruff, Biome) are
in [CONTRIBUTING.md](../CONTRIBUTING.md); [CI](#ci) lists what a pull request
runs beyond them.

## Hermetic suite

```sh
ctest --preset debug                      # everything, six tests at a time
ctest --preset debug -R '^core$'          # one CTest by name
```

The source contracts and measurement self-tests need no build; they run in
`.github/lint.sh` with the format checks.

Test presets exist for `debug`, `release`, `sanitize`, `tsan` and `coverage`.

| CTest name | Covers |
| --- | --- |
| `core` | C++ unit tests (`tests/unit/`, binary `uagent_tests`; Web Push encryption only with `UAGENT_WEB_PUSH=ON`) |
| `integration_runtime`, `_tools`, `_ui`, `_providers`, `_mcp`, `_agents`, `_sandbox` | end-to-end cases against the real binary with a scripted HTTP provider and a PTY (`tests/integration_<group>.py` or `tests/integration_<group>/`) |
| `integration_web`, `integration_management` | native web host |
| `behavior_eval` | scenario scores against the committed baseline |
| `benchmarks` | native micro-benchmarks; only with `UAGENT_BUILD_BENCHMARKS=ON` |

Beyond the runners, `tests/fixtures/` holds the provider route contract, saved
sessions and the evaluation and browser-runtime fixtures; `tests/package_contents.py`
validates a release archive and its bundled skill tree; `tests/reviewer_eval/`
holds labelled tool calls for the approval reviewer (`run.py` checks a model by
hand with an API key; the unit tests read its questions).

### Selecting cases

Both runners take `--list`, `--test NAME` (exact) and `-k SUBSTRING`. The
integration runner also takes `--group` and `-j`, and accepts `--test` and
`-k` more than once:

```sh
build/debug/uagent_tests --list
python3 tests/integration.py build/debug/uagent --group runtime --list
python3 tests/integration.py build/debug/uagent --group tools -j 8
python3 tests/integration.py build/debug/uagent -j 8      # every group
```

The groups are `runtime`, `tools`, `ui`, `providers`, `mcp`, `agents`,
`sandbox`, `web` and `management`; `--list` prints each case with its group.

- A new top-level `test_` function in a group's module registers itself.
- Each case has its own deadline; `UAGENT_TEST_TIMEOUT_SCALE` multiplies the
  deadlines on slow or instrumented builds.
- A failing case does not stop the run; the failures are listed at the end.
- A failed case is not run again, here or in the browser suite: one that
  passes only sometimes has found a race, in the test or in the app.
- `-j N` runs the selection in N processes. Every case has its own home and
  ports; runtime depends on the machine, build and selected cases.

### Sanitizers, coverage and fuzzers

```sh
cmake --preset sanitize && cmake --build --preset sanitize --parallel
ctest --preset sanitize                   # ASan and UBSan

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

The weekly `fuzz.yml` workflow does this and then searches for five minutes per
fuzzer and uploads the corpus it reached.

## Web tests

Run from `web/`:

```sh
npm test                 # Node unit tests (tests/*.test.js)
npm run typecheck
npm run test:browser     # Playwright against a native host, both browsers
npm run test:spec -- tests/phone-layout.spec.js   # one spec: Chromium, no retry, stops at a failure
```

Which binary and bundle a browser test drives:

- `tests/web_host.py` runs `UAGENT_TEST_BINARY` (CI sets it), or by default
  `../build/debug/uagent`.
- Locally the host serves `web/dist` from disk, so a web edit needs
  `npm run build` but no rebuild of the binary. In CI it serves the bundle
  embedded in the binary, as a release does, and the sheet engine the build
  copied beside the binary (`share/uagent/sheet`).

How the tests are isolated:

- Each test owns its host, temporary HOME and folder, mock provider, pairing
  cookie and output directory, and waits on visible state or an API condition
  rather than a fixed delay.
- The Vite dev server used by fixtures such as `action.spec.js` and
  `render-count.spec.js` takes a port
  derived from the checkout's path and never reuses a server already
  listening there, so a run cannot test another checkout's tree.

What a spec may assume about time:

- The composer sends and attaches nothing until the event stream has caught
  up. A Playwright click waits for its control to enable; `press("Enter")`,
  `setInputFiles` and a click scripted inside the page do not, and lose their
  input silently. Call `await online(page)` (`fixtures.js`) before the first
  of them, and again after a step that reconnects.
- A layer closed from the interface (a popover, a dialog) removes its history
  entry in a step back of its own. A spec that then navigates by
  `location.hash` first waits for `history.state?.layer` to be gone.
- Speed is recorded, not asserted: frame gaps across a reload go into an
  attachment (`reload-frame-gaps.json`) and never fail a run.
- The only fixed pauses are gestures: the long-press holds in
  `browser.spec.js`, and the pauses between the wheel and scroll steps in
  `scroll-stick.spec.js`.

Playwright has two projects. `chromium` runs every spec; `webkit` runs the
cross-browser selection in [playwright.config.js](../web/playwright.config.js),
including action admission and management flows. A failed test is not retried.
Four workers run locally and two in CI. Traces and screenshots of failures are
kept in `web/test-results`.

## Behavioral evaluation

`benchmarks/eval.py` runs declarative scenarios from
`benchmarks/scenarios/*.json`: a folder fixture, a prompt, a scripted
provider and the checks that define a good run (answer, files read, forbidden
tools, model rounds, batch width, deduplication, cumulative request and result
size, folder immutability). Results are compared with
`benchmarks/baselines/hermetic.json`. `benchmarks/run_trace.py` holds the run
timing and trace aggregation the eval and the audit share.
The [benchmark snapshot](../benchmarks/README.md#current-measurements) records
dated package sizes and local CPU samples; its SVG chart generator uses only
the Python standard library.

```sh
python3 benchmarks/eval.py build/debug/uagent --check
python3 benchmarks/eval.py build/debug/uagent --scenario parallel_batch
python3 benchmarks/eval.py build/debug/uagent --update   # review the diff
python3 benchmarks/eval.py --self-test
```

- A scenario's `tier` is `regression` (gates the build) or `capability`
  (reported only; the eval suggests promoting it once it passes).
- `variants` and `variant_env` compare settings within one scenario, for
  example `read_volume` (250/500/1000-line reads). A `compacted` variant fails
  if it scores below its `control`.
- Reports include rounds, idle rounds, failure categories, context size,
  latency and usage.

The provider is scripted, so the evaluation measures the harness, not model
quality.

## Measurement tools

`benchmarks/audit.py` reports request and schema sizes from a fresh HOME;
`behavior_eval` gates their growth per scenario. `--profile`, `--host` and
`--history` add local observations.

`benchmarks/session_metrics.py` summarizes real sessions from their journals
without retaining prompts or tool values: cohorts by `session.ready`
provenance (a journal without it is not reported), failed-call recovery, repeats,
argument issues, activity polls and turn outcomes.

```sh
python3 benchmarks/audit.py build/debug/uagent
python3 benchmarks/audit.py build/debug/uagent --profile --host --history ~/.uagent/history
python3 benchmarks/session_metrics.py --since 2026-08-01
python3 benchmarks/session_metrics.py --cohort ID --json /tmp/sessions.json
```

## CI

`.github/workflows/ci.yml` runs on pull requests, pushes to `master`, weekly
and by hand. `release.yml` also calls it for a tag whose commit has no
passing `master` run.

On a pull request, `.github/changes.py` selects jobs by the paths changed:

| Changed paths | Jobs |
| --- | --- |
| only `docs/`, `README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, `LICENSE` | `lint` |
| those and `web/` | `lint`, `web`, `web-browser` |
| anything else | native and web jobs, with the additional filters below |

For native changes, `clang-tidy` reads the changed translation units in one
job. Changes to headers, top-level CMake files, `.github/`, vendored code,
`.dockerignore` or `.clang-tidy` select all units across three runners, as
non-PR runs do.
`coverage` runs weekly and by hand only. The selection source is
[changes.py](../.github/changes.py); [ci.yml](../.github/workflows/ci.yml) defines
the jobs and their conditions.

The Linux build jobs reuse compiled objects through ccache
(`.github/actions/native-setup`), restored in every run and saved from
`master` and once per pull request. A newer push to a pull request cancels
the older run; runs on `master` are never cancelled, so each merge is tested
as it landed.

A pull request skips the slowest legs (Linux ARM64, macOS x86_64,
ThreadSanitizer and the remote bundle) unless it is heavy: it touches
`CMakeLists.txt`, `CMakePresets.json`, `.dockerignore`, `cmake/`, `deploy/`,
`third_party/`, `.github/`, `src/web/`, `src/browser/`, `include/web/` or
`include/browser/` (`heavy` in `.github/changes.py`).
Master, scheduled and manual runs always run them.

| Job | Runs |
| --- | --- |
| `web-dist` | The web bundle the native builds embed; skipped when only `web/` or documents changed |
| `build-and-test` | Release builds with Web Push on Linux x86_64 and macOS ARM64, and Linux ARM64 and macOS x86_64 on master and for heavy changes; `ctest`; packaging and the target's remote payload |
| `remote-bundle` | On master and for heavy changes: assembles the four payloads into one verified bundle, adds it to each release archive and checks the archives |
| `sanitizers` | `sanitize` preset, `ctest` |
| `thread-sanitizer` | On master and for heavy changes: `tsan` preset: `core`, `integration_runtime`, `integration_tools`, `integration_web` |
| `coverage` | Weekly and by hand: `core` and every integration group with a branch report; fails under 67% line coverage |
| `lint` | `.github/lint.sh`: clang-format, cpplint, Ruff and the source contracts |
| `clang-tidy` | clang-tidy, changed units on one runner or all units across three |
| `web` | Biome format check and lint, Node tests, bundle and size report, a release host with Web Push |
| `web-browser` | Playwright in Chromium and WebKit against that host, in Playwright's own image (browsers preinstalled), split across three runners |
| `image` | On non-PR runs, or when `deploy/`, `.dockerignore` or a workflow changes: builds the container image for x86_64 from the tested Linux installation and starts it under compose's seccomp profile |
| `CI result` | fails if any required job failed or was cancelled |

The `master requires CI` ruleset requires `CI result`, so a red run blocks a
merge. When a pull request stacked on another's branch is pushed together
with it, GitHub sends two events: the first run is cancelled within seconds
and its `CI result` reads failed. CodeQL runs in its own workflow on pushes
to `master` and weekly.

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
- Text expected in the terminal's working row must fit it at any elapsed
  time: the row clips its label to the columns its counters leave.
