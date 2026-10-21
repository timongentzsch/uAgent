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
| `compare_agents.py` | Footprint of µAgent beside other coding-agent CLIs: a JSON report and, with `--chart`, an SVG chart |
| `web.py` | Native web host footprint with disposable state and a mock provider |
| `bench_core.cc` | Source of `uagent_bench`, built with `UAGENT_BUILD_BENCHMARKS=ON` |
| `scenarios/` | One declarative JSON file per evaluation scenario |
| `baselines/` | Committed results: `hermetic.json` gates `eval.py`; `web.json` and `delegation.json` are reference measurements |
| `browser/` | One prompt through the `browser` tool of a web host ([README](browser/README.md)) |
| `delegation/` | Solo, subagent and panel runs of four tasks ([README](delegation/README.md)) |

## Footprint comparison

`compare_agents.py` measures each tool the same way, with runs alternating
between the tools so background load affects all of them:

- **Size:** the executable, and the installed tree (`cmake --install --strip`
  for µAgent: `bin/` and `share/uagent/`; the single versioned file for
  Claude Code; the cask directory for Codex CLI; the `node_modules` tree for
  omp). omp's sizes include the Bun runtime that runs it, as Claude Code's
  executable embeds its own.
- **`--version`:** 30 runs after 3 warm-ups; wall and CPU time.
- **Idle session:** 10 s in a 120×40 pseudo-terminal after the screen settles,
  with the provider's model list cached, at the folder-trust prompt where a
  tool asks. Resident memory is summed over the tool's own processes, helpers
  included. Codex CLI's interactive UI cannot skip the user config, so its row
  counts only Codex's own executables.
- **Headless turn:** one one-word prompt against the tool's real provider.
  Claude Code runs with `--strict-mcp-config`, an empty MCP config and only
  project settings; Codex CLI with `exec --ignore-user-config`; omp without
  extensions, skills, rules, LSP or a saved session, with its provider set
  through `--omp-env`.

Processes are attributed by PID: the started process, its descendants, and new
detached processes started from the tool's executable. Idle memory varies
between runs by tens of MB for the Node-based tools. Time to first screen ends
at the first half second without output, so a startup animation (omp's logo)
counts in full; the README's chart leaves it out.
