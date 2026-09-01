# Tools

µAgent builds the model-visible tool registry at startup and refreshes it when
MCP capabilities change. The exact set is filtered by approval policy, lean
mode, route capabilities, runtime state, installed skills, delegation depth,
and trusted configuration. `/context` prints the schemas currently advertised;
normal turn-boundary work can still change the next wire request.

## Core tools

| Tool | Purpose | Availability |
| --- | --- | --- |
| `read_path` | Read a known text file or bounded line range, or list a directory | standard and lean toolsets |
| `grep` | Search paths or file contents with a regex and optional glob | standard and lean toolsets |
| `write_file` | Create a file or replace it whole, including with empty content | standard toolset; mutating |
| `edit_file` | Apply ordered exact replacements atomically to an existing file | standard toolset; mutating |
| `attach` | Add a local image or document to the next model request | when attachments are enabled |
| `show_image` | Render a local image with the terminal's native inline protocol | interactive terminals with inline-image support |
| `run` | Execute a supervised shell command, optionally yielding, using a PTY, or detaching | execute capability |
| `scratch` | Create or rerun one bounded uv-backed scratch script | standard toolset with execute capability |
| `memory` | List, search, read, or explicitly mutate native memory; automatic changes produce private audit receipts | standard toolset when memory and policy allow it |
| `uagent_info` | Describe this build: version, flags, slash commands, configuration schema with effective values and provenance, the live tool surface, or the model routes and providers it can reach | standard toolset; inspect-only |
| `web_fetch` | Read one http(s) URL as text, converting markup to what a reader would see | standard toolset; approval required |

`read_path` decodes text only: a file whose first bytes are not text is refused
with a pointer to `attach` rather than decoded into replacement characters.

Filesystem and external-read approval follows the active path policy. Mutating
and process tools require approval unless yolo mode is active. Editing µAgent's
own configuration, the project trust store or `.mcp.json` is a stricter class:
it always asks, yolo does not apply, and a run with no interactive terminal
denies rather than assuming consent. Child processes receive the sanitized
environment described in [SECURITY.md](../SECURITY.md).

## Activity tools

These tools are advertised when supervised background work makes them useful:

| Tool | Purpose |
| --- | --- |
| `activity` | List activities, drain or wait for bounded output, write raw characters to a retained PTY, interrupt or resize it, or stop it |

`run` waits up to `UAGENT_RUN_YIELD_MS` (10 seconds by default) before a
still-running command becomes an activity. Explicit `yield_ms=0` waits to the
turn deadline; values from 250 through 30,000 override the initial wait. Set
`tty=true` only when the process needs interactive input.
Every `activity` call names one operation: `list`, `poll`, `wait`, `write`,
`resize`, or `stop`. `poll`, `write`, `resize`, and `stop` require an `id`;
`wait` requires a bounded `wait_ms` and optionally chooses `mode=any|all`.
`stop` terminates the complete process group and cleans its records and logs.
An empty `chars` value is a valid write with no input bytes, while `\u0003`
interrupts the process group. A resize requires `rows` and `cols` in 1..1000. Normal input to a non-TTY
activity is rejected. `run` and `activity` accept `max_output_chars` to lower
the host-capped output budget for one interaction.

Supervised PTY and non-TTY outputs share one event-driven process-I/O layer.
Once returned by `run` or `activity`, output is not returned again as new output. A 1 MiB head/tail
buffer preserves the oldest and newest bytes and reports an omitted middle.
Persistent detached commands remain log-based and cannot be interactively
reattached after the harness exits. Waiting readers use native kqueue/inotify
file notifications where available.

## Conditional and extensible tools

| Tool | Condition |
| --- | --- |
| `web_search` | an OpenRouter-protocol route or configured search endpoint is available |
| `subagent` | delegation is enabled and the current depth is below its limit |
| `skill` | at least one installed skill remains usable after tool-requirement filtering |
| `adapt_system` | `UAGENT_ADAPT_SYSTEM=1` |
| `uagent_configure` | the process is not a delegated child; persists a typed change to a registered setting after an exact diff is approved by a person |
| `<server>_<tool>` | discovered from a configured MCP server; names are sanitized and collision-safe |

`subagent` defaults to `operation=spawn`, returning both an activity ID and a
durable collaborator ID. `operation=followup` resumes that collaborator's
private conversation and prepends its persisted coordinator-owned `directive`;
an explicit empty directive clears it. `message` queues one-shot guidance for
the next follow-up, and `list` reports workspace collaborators. Use the
ordinary `activity` tool for live output, waiting, and stopping; collaboration
does not add a second process supervisor.

`web_fetch` is independent of hosted-route support. It decodes markup, JSON,
XML, and plain text, and refuses other content. Use the browser skill for pages
behind a login or assembled by scripts. Oversized bodies are truncated at
`UAGENT_WEB_FETCH_BYTES` and marked partial. For PDFs and images, download with
`run` and inspect with `attach`.

Only public Internet destinations are accepted. Before each initial or redirect
connection, µAgent checks every resolved IPv4 and IPv6 address. It rejects
loopback, private, carrier-NAT, link-local, reserved, and multicast addresses,
including IPv4-mapped, NAT64, Teredo, and 6to4 forms. Direct connections ignore
proxy environment variables so a proxy cannot resolve an unchecked address.

Every tool call is printed in full; results are summarized outside `/verbose`.

`web_search` is always one named model-facing function. The host implementation
calls OpenRouter's hosted `openrouter:web_search` server tool on a route of its
own, so models, yolo mode, and delegated workers receive the same schema,
citations, and accounting whichever model is answering. Set
`UAGENT_WEB_SEARCH_BACKEND=off` to withhold the tool.

Independent parallel-safe calls may execute concurrently, but results are
appended to the conversation in model call order. Tool-specific limits,
timeouts, visibility, stable-argument checks, and the global turn budget remain
host-enforced even in yolo mode.
