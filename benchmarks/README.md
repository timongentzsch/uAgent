# Benchmarks

Measurements of the agent's behavior and footprint. They are trend signals,
not correctness gates, except the hermetic evaluation, which CTest runs as
`behavior_eval`. Run from the repository root:

```sh
python3 benchmarks/eval.py build/debug/uagent --check    # scenarios against the baseline
python3 benchmarks/audit.py build/debug/uagent           # request and schema sizes
python3 benchmarks/web.py build/release/uagent --output web-footprint.json
cmake -B build/bench -DCMAKE_BUILD_TYPE=Release -DUAGENT_BUILD_BENCHMARKS=ON
cmake --build build/bench --target uagent_bench && build/bench/uagent_bench
```

[Testing](../docs/TESTING.md#behavioral-evaluation) covers the scenarios,
live runs, the cost authority and the other options.

| Path | Contents |
| --- | --- |
| `eval.py`, `live_authority.py`, `run_trace.py` | Scenario evaluation, the live-route authority check and the shared run timing |
| `audit.py`, `session_metrics.py` | Request-size report and a summary of real sessions from their journals |
| `web.py` | Native web host footprint with disposable state and a mock provider |
| `bench_core.cc` | Source of `uagent_bench`, built with `UAGENT_BUILD_BENCHMARKS=ON` |
| `scenarios/` | One declarative JSON file per evaluation scenario |
| `baselines/` | Committed results: `hermetic.json` gates `eval.py`; `web.json` and `delegation.json` are reference measurements |
| `browser/` | One prompt through the `browser` tool of a web host ([README](browser/README.md)) |
| `delegation/` | Solo, subagent and panel runs of four tasks ([README](delegation/README.md)) |
