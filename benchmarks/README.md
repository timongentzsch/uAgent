# Benchmarks

Measurements of the agent's behavior and speed. They are trend signals,
not correctness gates, except the hermetic evaluation, which CTest runs as
`behavior_eval`. Run from the repository root:

```sh
python3 benchmarks/eval.py build/debug/uagent --check    # scenarios against the baseline
cmake -B build/bench -DCMAKE_BUILD_TYPE=Release -DUAGENT_BUILD_BENCHMARKS=ON
cmake --build build/bench --target uagent_bench && build/bench/uagent_bench
```

[Testing](../docs/testing.md#behavioral-evaluation) covers the scenarios and
the other options.

| Path | Contents |
| --- | --- |
| `eval.py`, `run_trace.py` | Scenario evaluation and the shared run timing |
| `session_metrics.py` | A summary of real sessions from their journals |
| `bench_core.cc` | Source of `uagent_bench`, built with `UAGENT_BUILD_BENCHMARKS=ON`; `--session PATH` measures a saved conversation in place of the built long one |
| `scenarios/` | One declarative JSON file per evaluation scenario |
| `baselines/` | Committed results: `hermetic.json` gates `eval.py` |
