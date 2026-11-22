# Benchmarks

Measurements of the agent's behavior and footprint. They are trend signals,
not correctness gates, except the hermetic evaluation, which CTest runs as
`behavior_eval`. Run from the repository root:

```sh
python3 benchmarks/eval.py build/debug/uagent --check    # scenarios against the baseline
python3 benchmarks/audit.py build/debug/uagent           # request and schema sizes
cmake -B build/bench -DCMAKE_BUILD_TYPE=Release -DUAGENT_BUILD_BENCHMARKS=ON
cmake --build build/bench --target uagent_bench && build/bench/uagent_bench
```

[Testing](../docs/testing.md#behavioral-evaluation) covers the scenarios and
the other options.

| Path | Contents |
| --- | --- |
| `eval.py`, `run_trace.py` | Scenario evaluation and the shared run timing |
| `audit.py`, `session_metrics.py` | Request-size report and a summary of real sessions from their journals |
| `compare_agents.py` | Footprint of µAgent beside other coding-agent CLIs: a JSON report and, with `--chart`, an SVG chart |
| `dns_sink.py`, `egress_record.py` | A DNS that resolves nothing and logs every name asked for, and the situations µAgent is driven through under it |
| `first_request.py` | Records the first model request a harness sends and counts its tools and tokens |
| `bench_core.cc` | Source of `uagent_bench`, built with `UAGENT_BUILD_BENCHMARKS=ON`; `--session PATH` measures a saved conversation in place of the built long one |
| `scenarios/` | One declarative JSON file per evaluation scenario |
| `baselines/` | Committed results: `hermetic.json` gates `eval.py` |
| `results/`, `charts.py` | Dated measurement snapshots; the SVG layout and styling of the footprint chart |

## Current measurements

Snapshot: **2026-11-12**, production commit `f3489142`, runtime build
`4d62d764…9c9dd52`. [Raw results](results/2026-11-12.json) retain five
native benchmark samples per case and the hermetic summary. These are dated measurements, not floating claims about a later
checkout. Local CPU measurements use an Apple M1 Pro, macOS 27.0.1 and Apple
clang 21.0.0, Release `-O3`, Web Push enabled.

Package and web sizes were measured again on **2026-11-15** for 1.9.0, commit
`54a8c66a`, from the three installations of
[CI run 43712546879](https://github.com/timongentzsch/uAgent/actions/runs/43712546879)
([raw](results/packages-2026-11-15.json)).

| Target | Main binary | Complete installation | Remote binary | Remote transfer with resources |
| --- | ---: | ---: | ---: | ---: |
| Linux x86_64 | 7.52 MB | 20.04 MB | 4.96 MB | 2.37 MB |
| Linux ARM64 | 6.63 MB | 19.15 MB | 4.07 MB | 2.15 MB |
| macOS ARM64 | 10.81 MB | 23.33 MB | 8.25 MB | 4.32 MB |

MB is 1,000,000 bytes. Complete installation sums unpacked regular-file sizes,
including documentation, skills and **the three compressed gateway payloads**.
Remote binary excludes those resources; transfer includes the selected binary,
skills and licenses in one compressed archive. Filesystem allocation and system
libraries are not counted. No remote performance comparison between platforms
is implied by these size measurements.

The fresh hermetic evaluation passes **13 scenarios, 16 variants and 110
checks**, including the baseline and variant comparisons. A separate fresh-HOME
mock-provider probe advertises **13 tools / 10,413 schema bytes**, with a
2,922-character system message. It is the observed request, not every optional
tool or a count of billed provider tokens. The audit reads captured schemas
directly; no generated tool-reference file or personal profile is required.

Serializing 1,000 request payloads, in milliseconds, median of five Release
runs on 2026-11-12 at commit `f3489142`:

| Wire format | Full payload | Cached payload |
| --- | ---: | ---: |
| Chat Completions | 169.3 | 24.6 |
| Responses | 282.7 | 35.6 |
| Anthropic Messages | 252.9 | 51.8 |

This is local payload serialization, not model or network latency.
All five core runs use the same built-in workload without a personal session.
The web bundle's first JS/CSS fetch is **174,276 compressed bytes**; all shipped
web assets total **2,330,660 compressed bytes**, including optional renderers.
The sheet engine is installed beside the app and counted with the installation.

Reproduce the local measurements from the repository root:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
  -DUAGENT_BUILD_BENCHMARKS=ON
cmake --build build/bench --target uagent uagent_bench --parallel 4
python3 benchmarks/eval.py build/bench/uagent --check --report /tmp/eval.json
python3 benchmarks/audit.py build/bench/uagent --json /tmp/audit.json
for run in 1 2 3 4 5; do build/bench/uagent_bench > "/tmp/core-$run.txt"; done
(cd web && npm run size)
```

Build `web/dist` first as described in [Testing](../docs/testing.md#common-cases).
Changing the committed snapshot requires new measurements, not just a date
edit. Package figures come from summing regular-file members of
each complete release archive and checking the matching gateway manifest.

## Footprint comparison

Measured **2026-11-18** on Apple M1 Pro in a Linux ARM64 container, with 2
CPUs, 4 GiB memory and no additional swap. Each row is a median of five
interleaved runs of the same one-word prompt; all 25 headless turns
succeeded. Startup uses 30 runs following three warm-ups.
[Raw samples and provenance](results/comparison-2026-11-18.json) include
actual versions, all samples and the CI archive hash.

![Idle memory and CPU per headless turn for µAgent, Claude Code, Codex CLI, omp and OpenCode](../docs/images/comparison.svg)

| Tool | Measured version | Complete install (MB) | Idle memory (MiB) | CPU per headless turn (ms) |
| --- | --- | ---: | ---: | ---: |
| µAgent | 1.10.1 / `d60a16aa` | 19.3 | 27.1 | 29 |
| Claude Code | 2.1.327 | 371.8 | 160.6 | 605 |
| Codex CLI | 0.167.0 | 480.7 | 166.2 | 163 |
| omp | 18.18.6 + Bun 1.4.4 | 402.3 | 326.6 | 1715 |
| OpenCode | 1.19.6 | 212.7 | 585.0 | 2054 |

µAgent's install includes all three gateway payloads. The measured binary came
from [CI run 44144262016](https://github.com/timongentzsch/uAgent/actions/runs/44144262016),
mounted read-only at `/opt/uagent`; the runtime-only stage of the recipe
installed the other tools. All answered through one endpoint serving Claude
Haiku 5.5, except OpenCode, which that endpoint's Claude route refuses and
which answered on its `gpt-6-luna` route. OpenCode is the release its default
installer gives; OpenCode 2 installs separately and was not measured here. The
container shared the proxy's network namespace to reach its localhost
endpoint. Proxy CPU is outside the measured process tree.

CPU time measures local work, not model quality or response speed. Idle RSS
sums the tool's processes and helpers; shared pages can be counted more than
once. MiB = 1,048,576 bytes, while installation MB = 1,000,000 bytes. Detached
helper CPU uses the last observed OS process sample; sub-second work may fall
below that sampler's resolution. The current harness adds this to reaped CPU
usage, refuses to chart failed comparisons, and retains each raw sample.

Render this chart again without calling a provider:

```sh
python3 benchmarks/compare_agents.py \
  --from-report benchmarks/results/comparison-2026-11-18.json \
  --chart docs/images/comparison.svg
```

### Method

`compare_agents.py` measures each tool the same way, with runs alternating
between the tools so background load affects all of them:

- **Size:** the executable, and the installed tree (`cmake --install --strip`
  for µAgent: `bin/` and `share/uagent/`; the single versioned file for
  Claude Code and for OpenCode; the cask directory for Codex CLI; the
  `node_modules` tree for omp). omp's sizes include the Bun runtime that runs
  it, as Claude Code's and OpenCode's executables embed their own.
- **`--version`:** 30 runs after 3 warm-ups; wall and CPU time.
- **Idle session:** 10 s in a 120×40 pseudo-terminal after the screen settles,
  with the provider's model list cached, at the folder-trust prompt where a
  tool asks. Resident memory is summed over the tool's own processes, helpers
  included. Codex CLI's interactive UI cannot skip the user config, so its row
  counts only Codex's own executables.
- **Headless turn:** one one-word prompt. Claude Code runs with
  `--strict-mcp-config`, an empty MCP config and only project settings; Codex
  CLI with `exec --ignore-user-config`, or with only the provider config the
  container writes (`--codex-config`); omp without extensions, skills, rules,
  LSP or a saved session, with its provider set through `--omp-env`; OpenCode
  with `run --pure`, without external plugins.

Processes are attributed by PID: the started process, its descendants, and new
detached processes started from the tool's executable. Idle memory varies
between runs by tens of MB for the Node-based tools. Time to first screen ends
at the first half second without output, so a startup animation (omp's logo)
counts in full.

### In a container

`footprint.Dockerfile` builds µAgent from its checkout beside Claude Code,
Codex CLI, omp and OpenCode, all answering through one endpoint that serves
Claude Haiku 5.5 on the Responses and Anthropic Messages routes
(`footprint-run.sh` lists the paths; `OPENCODE_MODEL` names another model
for OpenCode where the endpoint refuses it that one). The current recipe installs the complete gateway bundle
through `install.sh`. For an unpublished checkout, put its verified matching
bundle in `remote-bundle/` before building; see
[building and installing from source](../docs/operations/install.md#build-and-install-from-source).
Codex, Bun, omp and OpenCode have pinned build arguments; Claude Code uses its
installer.
Retain the generated JSON's actual version strings with any new comparison.
Run it on an otherwise idle host with fixed limits:

```sh
docker build -f benchmarks/footprint.Dockerfile -t uagent-footprint .
docker run --rm --cpus=2 --memory=4g --memory-swap=4g --network host \
  -e PROXY_URL=http://127.0.0.1:8787 --env-file proxy-key.env \
  -v "$PWD/out:/out" uagent-footprint \
  --machine "YOUR CPU, Linux container, 2 CPU / 4 GiB"
```

`proxy-key.env` holds `PROXY_KEY=…`; keep it out of the repository. The chart
and the raw runs land in `out/`.

## Outside contacts and first request

Measured **2026-11-18** ([raw](results/egress-2026-11-18.json)) in a
container with no network whose only resolver is `dns_sink.py`: every name a
process asks for is logged and none resolves, so the log is the list of
hosts the software tried to reach.

`egress_record.py` drives µAgent through nine situations with its model
endpoint a scripted server on 127.0.0.1:

| Situation | Hosts asked for |
| --- | --- |
| The web host starts, a device pairs, the catalogue is read | none |
| A turn in the web client; a headless turn | none |
| A headless turn, the endpoint named by host | that endpoint |
| About: check for an update | `api.github.com` |
| Auto approval, an OpenRouter provider configured beside the conversation's | none |
| Auto approval, that provider named in `approval.reviewerModel` | `openrouter.ai` |
| The agent calls `web_fetch` | the page's host |
| A push to a subscribed device | `fcm.googleapis.com` |

A previewed page that asked for a font, a stylesheet, a script, an image, a
beacon and a fetch at six other hosts reached none of them.
[SECURITY.md](../SECURITY.md#what-leaves-the-machine) lists every contact,
and `tests/egress_test.py` holds the source to that list.

One headless turn of each tool from a fresh home, the model endpoint set to
`first_request.py` on 127.0.0.1, which saves the request and refuses it:

| Tool | Hosts asked for besides the endpoint | Tools | Tool tokens | Instruction tokens | Total |
| --- | --- | ---: | ---: | ---: | ---: |
| µAgent 1.10.1 | none | 12 | 2,525 | 627 | 3,152 |
| OpenCode 2.3.2 | `models.opencode.ai` | 13 | 3,509 | 2,148 | 5,657 |
| OpenCode 1.19.6 | `models.opencode.ai`, `registry.npmjs.org` | 10 | 5,014 | 1,890 | 6,904 |
| Claude Code 2.1.327 | `api.anthropic.com` | 12 | 7,522 | 3,987 | 11,509 |

Tokens are o200k_base counts of the first request that carries tools. Claude
Code defers some tools; refused that, it sends all 22 (21,495 tokens). The
tools' interactive sessions were not recorded, and each has settings that
turn its own contacts off.

```sh
docker run -d --name egress --network none --sysctl net.ipv4.ip_unprivileged_port_start=0 \
  -v "$PWD:/src" -v "$PWD/resolv.conf:/etc/resolv.conf:ro" IMAGE sleep infinity
docker exec -d egress python3 /src/benchmarks/dns_sink.py /tmp/dns.log
docker exec -w /src egress python3 benchmarks/egress_record.py build/debug/uagent /tmp/dns.log
```

`resolv.conf` holds `nameserver 127.0.0.1`; `IMAGE` is one that can run the
binary and Python 3.
