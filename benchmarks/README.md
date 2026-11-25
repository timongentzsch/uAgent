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

### v1.11.0 release measurements

The 2026-11-24 measurements use the published **1.11.0** archives at commit
`6a4e1c10`. Each archive matched the checksum in the release. Sizes below use
decimal MB (1,000,000 bytes). [Release and checksums](https://github.com/timongentzsch/uAgent/releases/tag/v1.11.0) · [Package measurements](results/packages-2026-11-24.json)

| Target | Main binary | Complete installation | Remote binary | Remote transfer with resources |
| --- | ---: | ---: | ---: | ---: |
| Linux x86_64 | 7.60 MB | 20.36 MB | 5.03 MB | 2.40 MB |
| Linux ARM64 | 6.63 MB | 19.39 MB | 4.14 MB | 2.18 MB |
| macOS ARM64 | 10.90 MB | 23.65 MB | 8.34 MB | 4.35 MB |

Complete installation sums unpacked regular files, including documentation,
skills and all three compressed gateway payloads. Remote transfer measures the
compressed archive containing the worker, skills and licenses. The totals
exclude filesystem allocation and system libraries.

The release web bundle's initial JavaScript and CSS total **192,520 bytes**
when gzip-compressed. All shipped web assets total **2,353,507 bytes** under
the same calculation, including optional renderers. These are build-time
compression estimates. The sheet engine ships beside the executable and counts
toward installation size. [Web asset breakdown](results/packages-2026-11-24.json)

A fresh-home headless turn of the installed Linux ARM64 release advertised
**13 tools**. The benchmark counts **2,852 tool-schema tokens** plus **627
system-instruction tokens**, or **3,479 combined**, using `o200k_base`.
For comparison across CLIs, the counter reserializes each tool list with
Python’s default JSON formatting. The compact tool array in this POST counted
2,396 tokens, or 3,023 with system instructions. Both estimates exclude
user-role environment context, the prompt and protocol framing. The full
Chat Completions POST was 13,829 bytes. A loopback script returned `ok`. [Captured request](results/first-request-2026-11-24.json)

With the same package and scripted Chat Completions reply, median process-tree RSS was
**26.6 MiB idle**, and a headless one-word turn used **21 ms of local CPU**.
All five idle sessions settled, each with two processes. All five headless
commands returned exactly `ok`. This solo probe mounted the package read-only
from the host. The Linux ARM64 container ran on Apple M1 Pro with 2 CPUs,
4 GiB RAM and external networking disabled. [Samples and provenance](results/release-probe-2026-11-24.json)

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

The release-tag hermetic evaluation passed **110/110 checks across 16
variants**, plus all 17 baseline and variant comparisons, with the package
installed on the container's Linux filesystem. The same package at the
same path on a Docker Desktop host bind passed 109/110. The background
activity’s sandboxed log pump received a permission error on that mount.
Copying the binary and resources onto Linux storage restored the expected
marker. [Both evaluation runs](results/release-probe-2026-11-24.json)

### Historical native samples

The 2026-11-12 snapshot used commit `f3489142`, runtime build
`4d62d764…9c9dd52`, on Apple M1 Pro with macOS 27.0.1 and Apple clang
21.0.0, Release `-O3`, with Web Push enabled. Its hermetic evaluation passed
13 scenarios, 16 variants and 110 checks. Its separate fresh-home request probe
measured 13 tools, 10,413 schema bytes and a 2,922-character system message.
[Raw native and evaluation results](results/2026-11-12.json) · [Earlier package measurements](results/packages-2026-11-15.json)

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

The **2026-11-24** run measured five Linux ARM64 CLIs on an Apple M1 Pro in
a container limited to 2 CPUs and 4 GiB of memory. External networking was
disabled. Each CLI received the same prompt and a scripted one-word answer
over loopback. The table gives the median of five samples per CLI and metric.
[All samples, versions and distribution hashes](results/comparison-scripted-2026-11-24.json)

![Idle process-tree memory and client CPU per scripted one-word turn, measured November 24](../docs/images/comparison-scripted.svg)

| Tool | Version | Idle memory (MiB) | CPU per scripted turn (ms) |
| --- | --- | ---: | ---: |
| µAgent | 1.11.0 | 27.3 | 21 |
| Claude Code | 2.1.331 | N/A | 409 |
| Codex CLI | 0.172.0 | 176.9 | 147 |
| omp | 19.1.1 + Bun 1.4.5 | 337.1 | 1,773 |
| OpenCode | 1.19.6 | 569.2 | 2,296 |

Claude Code exited before settling in each of five offline interactive
starts. A diagnostic rerun reported a DNS failure for `api.anthropic.com`. Its idle memory is unavailable. The other four CLIs settled in all
five samples. The recorded startup diagnostics are in the report.

µAgent and Codex CLI used the Responses API. Claude Code, omp and OpenCode
used Anthropic Messages. Each of the 25 headless turns returned exactly
`ok`. OpenCode made one additional small-model request per turn. The server
handled 30 requests in total and performed no inference. Client CPU includes
the work of those extra requests. This measures local client overhead for a
short text reply. [Scripted server](scripted_provider.py)

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

### Reproduce the scripted comparison

Build the image on Linux ARM64 or pass `--platform linux/arm64` to Docker.
Select the published µAgent package to match the recorded release. The
Dockerfile pins the comparator versions and includes omp's dependency lock.
Use an otherwise idle host, and make the output directory writable by the
image's `bench` user.

```sh
docker build --platform linux/arm64 -f benchmarks/footprint.Dockerfile \
  --build-arg UAGENT_STAGE=published -t uagent-footprint .
mkdir -p out
docker run --rm --init --platform linux/arm64 --network=none \
  --cpus=2 --memory=4g --memory-swap=4g -v "$PWD/out:/out" \
  uagent-footprint --scripted --machine "YOUR CPU, Linux ARM64 container, 2 CPU / 4 GiB"
```

The container writes raw samples to `out/footprint.json` and exits nonzero if
a metric fails, as Claude Code idle did in the recorded run. Inspect the
errors before rendering a partial report. These commands reproduce the
published chart, including its unavailable value.

```sh
python3 benchmarks/compare_agents.py \
  --from-report benchmarks/results/comparison-scripted-2026-11-24.json \
  --chart docs/images/comparison-scripted.svg
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

The published 1.11.0 Linux ARM64 release was measured **2026-11-24** in a
network-isolated container. Its resolver, `dns_sink.py`, logged DNS queries
and pointed IPv4 lookups at a loopback sink. Direct IP connections do not
appear in a DNS log. [Release DNS results and method](results/egress-2026-11-24.json)

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
method as the v1.11.0 release probe on the first request carrying tools. Claude
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
