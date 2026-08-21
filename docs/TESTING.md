# Testing

The default suite is hermetic and needs no API key:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
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

The end-to-end efficiency harness has a hermetic smoke mode that runs in CTest
and a separately authorized live mode. Both compare the same read-only task
with compaction disabled and forced mid-turn compaction, then report task
quality, requests, tool calls, tokens/cache, cost, wall time, peak RSS, request
and schema size, and binary size:

```sh
python3 benchmarks/agent_efficiency.py build/debug/uagent
python3 benchmarks/agent_efficiency.py build/release/uagent --run \
  --model provider/model --model another/model --report /tmp/uagent-efficiency.json
```

Live runs are billable, require `--run`, and apply `--max-cost` (default
`$0.10`) to each isolated run. Providers that do not report cost cannot make a
dollar cap authoritative; the report marks their cost unavailable. A compacted
run fails if its behavioral score is below its control.

Keep tests proportional: pure helpers get focused unit coverage; externally
visible behavior gets one hermetic integration path. Avoid duplicating the
same contract across unit, integration, simulation, and live-model layers.
Never put secrets in prompts, fixtures, reports, or failures.
