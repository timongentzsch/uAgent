# Benchmarks

Measurements of the agent's behavior and footprint. They are trend signals,
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
| `compare_agents.py` | Footprint of µAgent beside other coding-agent CLIs: a JSON report and, with `--chart`, an SVG chart |
| `dns_sink.py`, `egress_record.py` | A loopback DNS sink that logs lookups, and the scenarios that exercise µAgent under it |
| `first_request.py`, `release_probe.py` | Count tools and instructions in captured requests, and measure an installed µAgent release against a scripted loopback reply |
| `bench_core.cc` | Source of `uagent_bench`, built with `UAGENT_BUILD_BENCHMARKS=ON`; `--session PATH` measures a saved conversation in place of the built long one |
| `scenarios/` | One declarative JSON file per evaluation scenario |
| `baselines/` | Committed results: `hermetic.json` gates `eval.py` |
| `results/`, `charts.py` | Dated measurement snapshots; the SVG layout and styling of the footprint chart |

## Current measurements

### v1.12.0 release measurements

The published v1.12.0 archives at commit `d312c15c` matched the release
checksums on 2026-11-24. Sizes use decimal MB (1,000,000 bytes).
[Release and checksums](https://github.com/timongentzsch/uAgent/releases/tag/v1.12.0) · [Exact bytes and hashes](results/packages-v1.12.0.json)

| Target | Download (MB) | Executable (MB) | Installed (MB) | SSH gateway transfer (MB) |
| --- | ---: | ---: | ---: | ---: |
| Linux x86_64 | 15.13 | 7.78 | 20.76 | 2.47 |
| Linux ARM64 | 14.90 | 6.82 | 19.80 | 2.25 |
| macOS ARM64 | 17.09 | 11.05 | 24.03 | 4.43 |

Installed size sums regular files, including documentation, skills and all
three compressed gateway payloads. An SSH workspace receives the gateway
archive for its target. System libraries and filesystem allocation are
excluded. The initial web JavaScript and CSS totaled **206,393 bytes**
under gzip. All shipped web assets, including optional renderers, totaled
**2,368,284 bytes** under the same build-time estimate.

The solo probe used a Linux ARM64 container on Apple M1 Pro with two CPUs,
4 GiB of memory and external networking disabled. Across five trials,
median idle memory was **26.7 MiB** (RSS summed across two processes), and
median local CPU time per headless turn was **17.8 ms**. Each turn returned
the scripted loopback reply `ok`. With the package on the container's own
filesystem, the hermetic evaluation passed 110/110 checks across 16 variants and all 17 baseline and variant
comparisons. [Samples and environment](results/release-probe-1.12.0-linux-arm64.json)

The first Chat Completions request in a fresh home advertised **13 tools**
and carried 13,829 bytes of JSON. With `o200k_base`, the compact tool array
(UTF-8 JSON without formatting whitespace) used 2,396 tokens and the
system instructions used 627, totaling **3,023 tokens**. For comparisons
across CLIs, the counter below uses Python's default JSON formatting for the
tool array, yielding **3,479 tokens** including the instructions. Both counts exclude user-role context, the
prompt and protocol framing. [Captured request](results/first-request-1.12.0-linux-arm64.json)

To repeat the release probe, extract the verified Linux ARM64 archive first.
Set `PACKAGE_DIR` to the extracted directory containing `bin/` and `share/`.
Use a Linux ARM64 runtime image with Python 3.11+, Git, procps and the release's
system libraries. The final stage of `footprint.Dockerfile` supplies these
dependencies. Give its container user write access to the output directory.

```sh
mkdir -p out
docker run --rm --init --platform linux/arm64 --cpus=2 --memory=4g \
  --memory-swap=4g --network=none -w /work --entrypoint python3 \
  -v "$PACKAGE_DIR:/opt/uagent:ro" -v "$PWD:/src:ro" -v "$PWD/out:/out" \
  "$RUNTIME_IMAGE" /src/benchmarks/release_probe.py /opt/uagent/bin/uagent /out \
  --machine "YOUR CPU, Linux ARM64 container, 2 CPU / 4 GiB"
uv run --with tiktoken==0.14.0 python benchmarks/first_request.py count out/first-request.json
```

### Historical native samples

The 2026-11-12 snapshot used commit `f3489142`, runtime build
`4d62d764…9c9dd52`, on Apple M1 Pro with macOS 27.0.1 and Apple clang
21.0.0, Release `-O3`, with Web Push enabled. Its hermetic evaluation passed
13 scenarios, 16 variants and 110 checks. Its separate fresh-home request probe
measured 13 tools, 10,413 schema bytes and a 2,922-character system message.
[Raw native and evaluation results](results/2026-11-12.json)

Local time to serialize 1,000 request payloads, median of five runs in milliseconds:

| Wire format | Full payload | Cached payload |
| --- | ---: | ---: |
| Chat Completions | 169.3 | 24.6 |
| Responses | 282.7 | 35.6 |
| Anthropic Messages | 252.9 | 51.8 |

Each run used the same built-in workload. Model inference and network latency
are outside this measurement.

Reproduce the local measurements from the repository root:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
  -DUAGENT_BUILD_BENCHMARKS=ON
cmake --build build/bench --target uagent uagent_bench --parallel 4
python3 benchmarks/eval.py build/bench/uagent --check --report /tmp/eval.json
for run in 1 2 3 4 5; do build/bench/uagent_bench > "/tmp/core-$run.txt"; done
(cd web && npm run size)
```

Build `web/dist` first as described in [Testing](../docs/testing.md#common-cases).
For historical timings, use the recorded source revision and compiler. The
release package figures sum regular-file members of each archive and use the
matching gateway manifest.

## Footprint comparison

The **2026-11-24** run compared five CLIs on Apple M1 Pro in a
Linux ARM64 container with 2 CPUs, 4 GiB of memory and external networking
disabled. The table gives medians of five settled interactive sessions and
five headless turns per CLI. Separate passes interleaved the five tools for
idle and headless measurements. Every headless turn returned exactly `ok` from
a scripted loopback provider.
[Samples, versions and distribution hashes](results/comparison-scripted-1.12.0-linux-arm64.json)

![Idle process-tree memory and client CPU per scripted one-word turn](../docs/images/comparison-scripted-1.12.0-linux-arm64.svg)

| Tool | Version | Idle memory (MiB) | CPU per scripted turn (ms) |
| --- | --- | ---: | ---: |
| µAgent | 1.12.0 | 27.4 | 19 |
| Claude Code | 2.1.331 | 217.9 | 374 |
| Codex CLI | 0.172.0 | 165.1 | 110 |
| omp | 19.1.1 + Bun 1.4.5 | 345.1 | 1,577 |
| OpenCode | 1.19.6 | 700.9 | 2,231 |

Claude Code used a disposable gateway bearer credential. The harness completed
normal onboarding and folder trust, typed a draft in the command composer
and cleared it before each idle window. [Startup evidence](results/claude-startup-2.1.331.json) · [Claude Code gateway authentication](https://code.claude.com/docs/en/llm-gateway-connect)

µAgent and Codex CLI used the Responses API. Claude Code, omp and OpenCode
used Anthropic Messages. OpenCode made one extra small-model request per turn.
The fixture answered all 30 requests locally without model inference. [Scripted provider](scripted_provider.py)

### Earlier live-provider comparison

Measured **2026-11-18** on Apple M1 Pro in a Linux ARM64 container, with 2
CPUs, 4 GiB memory and no additional swap. Each row is a median of five
interleaved runs asking for the same one-word reply. All 25 headless commands
exited successfully and passed the harness output check. Startup uses 30 runs
following three warm-ups.
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
which answered on its `gpt-6-luna` route. The run used the OpenCode 1.19.6
default installer and excluded the separate 2.x distribution. The
container shared the proxy's network namespace to reach its localhost
endpoint. Proxy CPU is outside the measured process tree.

CPU time measures local work, not model quality or response speed. Idle RSS
sums the tool's processes and helpers; shared pages can be counted more than
once. MiB = 1,048,576 bytes, while installation MB = 1,000,000 bytes. Detached
helper CPU uses the last observed OS process sample; sub-second work may fall
below that sampler's resolution. The harness adds observed detached-helper
CPU to reaped CPU usage and retains each sample. In this run, success required
a zero exit status and `ok` somewhere in combined output. The historical
report does not retain response text.

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
  with the provider's model list cached. For the scripted run Claude Code
  reaches its composer. Other tools can settle at a folder-trust prompt.
  Resident memory is summed over the tool's own processes, helpers
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

### Reproduce the scripted comparison

Use a clean checkout with this report. Build the image from harness revision
`4bb3df93`, which pins the comparator versions, then return to the checkout
with the raw samples. Use a Linux ARM64 builder or pass `--platform linux/arm64` to Docker.
Use an otherwise idle host, and make the output directory writable by the
image's `bench` user.

```sh
git switch --detach 4bb3df93fe3bc1712348af5f030acce89422ed12
docker build --platform linux/arm64 -f benchmarks/footprint.Dockerfile \
  --build-arg UAGENT_STAGE=published --build-arg UAGENT_VERSION=1.12.0 -t uagent-footprint .
git switch -
mkdir -p out/idle out/headless
run_footprint() {
  output_dir=$1
  shift
  docker run --rm --init --platform linux/arm64 --network=none \
    --cpus=2 --memory=4g --memory-swap=4g -v "$PWD/out/$output_dir:/out" \
    uagent-footprint --scripted --machine "YOUR CPU, Linux ARM64 container, 2 CPU / 4 GiB" "$@"
}
run_footprint idle
run_footprint headless --skip idle --version-runs=0 --warmups=0 --session-runs=5
```

Use the idle samples from `out/idle/footprint.json` and the CPU samples from
`out/headless/footprint.json`. Each run exits nonzero if a metric fails.
The published samples can also regenerate the chart.

```sh
python3 benchmarks/compare_agents.py \
  --from-report benchmarks/results/comparison-scripted-1.12.0-linux-arm64.json \
  --chart docs/images/comparison-scripted-1.12.0-linux-arm64.svg
```

The loopback server speaks both Responses and Anthropic Messages. For a live-provider
run, omit `--scripted`, enable network access to the provider and supply
`PROXY_URL` and `PROXY_KEY` as environment variables. Save the actual model
routes and tool versions alongside the result. The earlier live-provider
comparison used an alternate OpenCode model.

To measure a local checkout, omit `UAGENT_STAGE=published` and supply the
verified matching gateway bundle as described in the install guide.
[Building from source](../docs/operations/install.md#build-and-install-from-source)

## Outside contacts and first request

The published v1.12.0 Linux ARM64 release ran in a network-isolated
container. Its loopback resolver recorded DNS requests. Direct IP
connections and live-browser traffic fall outside this probe. [v1.12.0 DNS results](results/egress-1.12.0-linux-arm64.json)

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

A separate 2026-11-18 browser probe requested a font, stylesheet, script,
image, beacon and fetch. None of the six external names appeared in its DNS
log. [Historical browser and CLI capture](results/egress-2026-11-18.json)
[SECURITY.md](../SECURITY.md#what-leaves-the-machine) lists every contact,
and `tests/egress_test.py` checks the documented source-level host inventory.

The 2026-11-18 headless comparison used a fresh home for each tool and
`first_request.py` on 127.0.0.1 to capture the request and refuse it.

| Tool | Hosts asked for besides the endpoint | Tools | Tool tokens | Instruction tokens | Total |
| --- | --- | ---: | ---: | ---: | ---: |
| µAgent 1.10.1 | none | 12 | 2,525 | 627 | 3,152 |
| OpenCode 2.3.2 | `models.opencode.ai` | 13 | 3,509 | 2,148 | 5,657 |
| OpenCode 1.19.6 | `models.opencode.ai`, `registry.npmjs.org` | 10 | 5,014 | 1,890 | 6,904 |
| Claude Code 2.1.327 | `api.anthropic.com` | 12 | 7,522 | 3,987 | 11,509 |

The historical token columns use the same normalized `o200k_base` counting
method as the release probe on the first request carrying tools. Claude
Code deferred some tools. After a refusal it sent all 22 (21,495 tokens).
This snapshot covers the named headless versions and configurations.
The 2026-11-18 report retains aggregate counts and DNS host lists. The
exact µAgent egress checkout and individual request bodies are unrecorded.

```sh
docker run --rm --init --platform linux/arm64 --network=none \
  --sysctl net.ipv4.ip_unprivileged_port_start=0 --entrypoint sh \
  -v "$PACKAGE_DIR:/opt/uagent:ro" -v "$PWD:/src:ro" \
  -v "$PWD/resolv.conf:/etc/resolv.conf:ro" "$RUNTIME_IMAGE" -ec '
    python3 /src/benchmarks/dns_sink.py /tmp/dns.log &
    python3 /src/benchmarks/egress_record.py /opt/uagent/bin/uagent /tmp/dns.log'
```

`resolv.conf` holds `nameserver 127.0.0.1`. Use the same package directory
and runtime image as the release probe.

## Live memory evaluation

The November 24 memory evaluation used DeepSeek V4 Flash through OpenRouter.
Each mode received a disposable home, two synthetic projects and the same
correction to a saved preference. With memory on, the model saved and recalled
the correction in one of two completed pairs. In the repeat the extractor
proposed no change and retained the old note. With memory off it answered
`UNKNOWN` both times. Both modes answered `UNKNOWN` in the unrelated project.
Both reread the dispatch fixture
after its first six lines omitted the requested code, returned the exact code
and included extra formatting or text. These are individual model responses.

Four additional fixtures tested deliberate deletion using independently worded
historical conversations. Before the fix, three restored the forgotten fact
under a different key. Across two runs of all four fixtures after the fix,
the runtime declined six proposed keys and two extractions proposed no note.
All eight libraries stayed empty. One additional paired attempt stopped on
a nonzero process exit and yielded no quality score.
The report includes the native binary hashes, tool sequences and extraction
receipts. [Before and after results](results/memory-quality-2026-11-24.json)

Run the paid provider evaluation with a configured model. The script uses
only that provider's credentials and saves aggregate observations.

```sh
python3 benchmarks/memory_quality.py build/dev/uagent \
  --model openrouter/deepseek/deepseek-v4-flash --report /tmp/memory-quality.json
```
