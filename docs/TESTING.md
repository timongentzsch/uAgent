# Testing

The default suite is hermetic and needs no API key:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`tests/unit/` covers local policy and protocols, including activity IDs,
head/tail buffering and LRU retention, atomic pre-spawn admission, default
yielding, event-driven PTY and pipe input/output, split readiness markers, exactly-once completion delivery,
200 Ctrl+B/exit races, parallel foreground handoff, PTY input/resize, non-TTY
rejection, and resumed-image matching. Provider fixtures cover structured and
proxy-wrapped error classes, one-shot context compaction, observational
background completion, bounded task delivery, and no-replay failure paths. One shared HTTP/PTY fixture drives isolated
`runtime`, `tools`, `ui`, `providers`, `mcp`, and `delegation` CTest processes.
Fuzz targets run in CI.

Keep tests proportional: pure helpers get focused unit coverage; externally
visible behavior gets one hermetic integration path. Avoid duplicating the
same contract across unit, integration, simulation, and live-model layers.
Never put secrets in prompts, fixtures, reports, or failures.
