# Operations

µAgent targets one foreground POSIX CLI session on macOS or Linux. Windows,
daemon use, multi-tenant hosting, and HTTP MCP require separate security and
process designs.

## Default guardrails

These are local safety bounds, not provider service objectives.

| Concern | Default |
| --- | ---: |
| first event / stream idle / request | 120 / 90 / 300 s |
| complete turn | 900 s |
| model rounds / tool calls | 40 / 100 |
| reported turn cost | $1.00 |
| request / response | 64 / 32 MiB |
| project instructions | 32 KiB |
| attachment / terminal image | 10 / 10 MiB |
| tool result | 8,000 characters |
| grep | 200 matches, 8,000 characters |
| background jobs / safe tool workers | 8 / 4 |
| web search | 2 calls, 4 queries/call, 1,200 output tokens |
| MCP servers / tools per server | 32 / 256 |
| checkpoint suggestion / urgent | 65% / 85% |
| emergency compaction | 95% |
| saved removed-trace archive | newest 16 MiB |

Other bounds include 4 MiB assembled reads, 10 MiB editable files, 64 MiB
rotating shell logs, 1 MiB MCP config, 16 MiB MCP response/log, and 256 KiB
schema per server. `run_python` shares the shell job/log bounds, caps source at
128 KiB and accepts at most 12 package requirements. Raise limits only with a
representative workload and memory/request measurement.

Detached-terminal records older than `UAGENT_TERMINAL_DAYS=7` are pruned when
listed. Their rotating logs remain bounded independently.

## Release check

GitHub Actions builds and tests Debug and Release natively on Linux x86_64,
Linux arm64, and macOS arm64. Release jobs upload versioned archives and SHA-256
files. A `v<project version>` tag publishes those assets as a GitHub Release;
other version tags fail before publication.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
UAGENT_BUILD_DIR=/tmp/uagent-build UAGENT_PREFIX=/tmp/uagent-prefix ./install.sh
/tmp/uagent-prefix/bin/uagent --version
```

Also:

1. Build Debug and Release with warnings as errors.
2. Run ASan/UBSan on Linux.
3. Compare `uagent_bench` on the same machine and build type.
4. Exercise one real provider turn, the default isolated Chrome MCP, its user
   attach mode, and each configured MCP server. Record the npm version resolved
   by `chrome-devtools-mcp@latest` when diagnosing or releasing.
5. Inspect a private debug trace for resolved model/window, time to first
   event, tool duration, cache usage, cost, limits, and terminal outcome.
6. Run `python3 tests/agent_workflow_live.py --run` after orchestration,
   routing, checkpoint, or delegation changes.
7. Before deploying a new model route, run correction, failure, multi-fold,
   and large-context checkpoint cases in `shadow`, then promote it only after
   exact-fact and no-mutation checks pass.

Investigate repeatable benchmark regressions above 10%; do not gate on one
noisy run. Provider tests should capture p50/p95/p99 latency, errors, degraded
features, uncached/cached/write tokens, cost, and peak RSS.

## Failure handling

- Headless automation relies on nonzero exit plus stderr; limits and empty
  answers must not appear successful.
- MCP failures are server-local. Inspect `~/.uagent/mcp/<name>.log`.
- Debug traces under `~/.uagent/sessions` can contain private source and model
  reasoning; logging is off by default.
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
