# Operations

µAgent targets one foreground POSIX CLI session on macOS or Linux. Windows,
daemon use, multi-tenant hosting, and HTTP MCP require separate security and
process designs.

## Default guardrails

These are local safety bounds, not provider service objectives.

| Concern | Default |
| --- | ---: |
| first event / stream idle / request | 120 / 90 / 300 s |
| transient request retries / base backoff | 2 / 0.5, 1 s with jitter |
| complete turn | 900 s |
| model rounds / tool calls | 40 / 100 |
| subagent depth / child rounds / calls | 2 / 25 / 60 |
| reported turn cost | $1.00 |
| request / response | 64 / 32 MiB |
| project instructions and memory index | 32 KiB |
| memory entry / memories per scope | 2 KiB / 32 |
| skill body / description / count | 16 KiB / 512 B / 64 |
| attachment / terminal image | 10 / 10 MiB |
| attachments pending per step | 8 |
| generic tool result / batch content | 8,000 / 16,000 characters |
| source read | 1,000 default / 10,000 max lines; 32 KiB |
| small-directory inspection | 4 regular text files / 32 KiB |
| grep | 200 matches, 8,000 characters |
| background jobs / safe tool workers | 8 / 4 |
| OpenRouter server search | 5 results/search, 3 searches/request |
| MCP servers / tools per server | 32 / 256 |
| checkpoint suggestion / urgent | 65% / 85% |
| emergency compaction | 95% |
| saved removed-trace archive | newest 16 MiB |

Other bounds include 10 MiB editable files, 64 ordered edits per `edit_file`
call, 64 MiB rotating shell logs, 1 MiB MCP config, 16 MiB
MCP response/log, and 256 KiB schema per server. `run_python` shares the shell
job/log bounds, caps source at 128 KiB and accepts at most 12 package
requirements. Raise limits only with a representative workload and
memory/request measurement.

The generic batch bound is twice `UAGENT_TOOL_RESULT_CHARS`. A tool or per-call
result with an explicit larger window raises a mixed batch to that one window;
parallel reads share it. Protocol envelopes and JSON escaping are additional.
A complete workspace-local directory listing also includes file contents when
it contains at most four regular non-symlink text files within the same
source-read bound; external, nested, binary, symlinked, larger, or paginated
listings remain names-only.

Child processes omit credential-like environment variables. If an approved
shell command needs one, list its exact name in
`UAGENT_SHELL_ENV_ALLOW=GH_TOKEN,SSH_AUTH_SOCK`. This exception applies only to
the `run` tool; MCP, delegation, and Python subprocesses stay sanitized.

Detached-terminal records older than `UAGENT_TERMINAL_DAYS=7` are pruned when
listed. Their rotating logs remain bounded independently.

## Release check

GitHub Actions builds and tests Debug and Release natively on Linux x86_64,
Linux arm64, and macOS arm64. Linux jobs add ASan/UBSan, TSan, short
deterministic parser fuzzing, and branch-coverage artifacts. Release jobs upload
versioned archives and SHA-256 files. A `v<project version>` tag publishes those
assets as a GitHub Release; other version tags fail before publication. A
separate style job enforces the Google formatter, selected semantic/analyzer
checks, and `cpplint` on first-party C++.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
UAGENT_BUILD_DIR=/tmp/uagent-build UAGENT_PREFIX=/tmp/uagent-prefix ./install.sh
/tmp/uagent-prefix/bin/uagent --version
```

Also:

1. Build Debug and Release with warnings as errors.
2. Run ASan/UBSan and TSan on Linux; smoke the standalone parser fuzzers.
3. Inspect branch coverage for newly changed failure and cancellation paths.
4. Compare `uagent_bench` on the same machine and build type.
5. Exercise one real provider turn, the default isolated Chrome MCP, its user
   attach mode, and each configured MCP server. Record the npm version resolved
   by `chrome-devtools-mcp@latest` when diagnosing or releasing.
6. Inspect a private debug trace for resolved model/window, time to first
   event, tool duration, cache usage, cost, limits, and terminal outcome.
7. Run `python3 tests/agent_workflow_live.py --run` after orchestration,
   routing, checkpoint, or delegation changes.
8. Before deploying a new model route, run correction, failure, multi-fold,
   and large-context checkpoint cases in `shadow`, then promote it only after
   exact-fact and no-mutation checks pass.

For a fair model A/B, repeat `--model`; each candidate receives a fresh
workspace, home, session, and fixture. Keep the JSON report with the change:

```sh
python3 tests/agent_workflow_live.py --run --scenario analysis \
  --model deepseek/deepseek-v4-flash \
  --model stepfun/step-3.7-flash \
  --repetitions 3 \
  --effort low \
  --report /tmp/uagent-model-ab.json
```

Each trial receives a fresh workspace, home, session, and fixture. The report
records contract violations, workspace and binary provenance, provider/tool
timing, failures, dependency installs, exploration strategy, tokens, cost,
wall time, and peak RSS. The cost ceiling is per model, so one candidate cannot
consume another's budget.

The research scenario deliberately disables OpenRouter server search to test
µAgent's compatibility fallback. It batches two queries into one call and uses
minimal side-search effort; production keeps the model-decided server tool.

Investigate repeatable benchmark regressions above 10%; do not gate on one
noisy run. Provider tests should capture p50/p95/p99 latency, errors, degraded
features, uncached/cached/write tokens, cost, and peak RSS.

## Failure handling

- Headless automation relies on nonzero exit plus stderr; limits and empty
  answers must not appear successful.
- MCP failures are server-local. Inspect `~/.uagent/mcp/<name>.log`.
- Debug traces under `~/.uagent/sessions` can contain private source and model
  reasoning; logging is off by default.
- Memories are model-written. Only their name/scope index loads automatically;
  bodies are read on demand as non-authoritative evidence. Review
  `<base>/memory` before sharing it and use `memory(forget=true)` to retract one.
- Skill names and descriptions are returned only through deferred discovery;
  bodies arrive only when opened. A skill with no `description` is skipped.
- A workspace `.uagent/.config` is ignored until trusted. Headless runs warn and
  fall back to `~/.uagent/.config` rather than failing.
- A model that refuses image input degrades for the current route: image parts
  are dropped, paths and non-image attachments remain, and later images are
  refused. A reset or route change probes image support again. A route's
  advertised modalities can be wrong, so this is detected rather than declared.
- Session archives drop oldest segments when full and record the count.
- A failed checkpoint reread is recorded and the fold continues. Switch back
  to `shadow` if continuation quality regresses.
- Provider preference may improve locality but expose a provider outage or
  rate limit. Default sticky routing with fallbacks is safer.
- Reported-cost enforcement happens between calls; an in-flight concurrent
  batch can overshoot because cost is known only after completion.
- Managed process groups are cleaned on normal/catchable exits. No program can
  guarantee cleanup after `SIGKILL`; inspect the process table and private logs.

The repository contains no production incident history or external latency and
availability targets. If they exist elsewhere, convert them into executable
checks and replace these assumptions.
