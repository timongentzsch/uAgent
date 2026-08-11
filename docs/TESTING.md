# Testing

The default suite is hermetic and needs no API key:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`tests/unit/` covers local policy and protocols. One shared HTTP/PTY fixture
drives isolated `runtime`, `tools`, `ui`, `providers`, `mcp`, and `delegation`
CTest processes. Fuzz targets run in CI.

Keep tests proportional: pure helpers get focused unit coverage; externally
visible behavior gets one hermetic integration path. Avoid duplicating the
same contract across unit, integration, simulation, and live-model layers.
Never put secrets in prompts, fixtures, reports, or failures.
