# Tools

µAgent builds the model-visible tool registry at startup and refreshes it when
MCP capabilities change. The exact set is filtered by approval policy, lean
mode, route capabilities, runtime state, installed skills, delegation depth,
and trusted configuration. `/context` prints the schemas advertised for the
next request.

## Core tools

| Tool | Purpose | Availability |
| --- | --- | --- |
| `read_path` | Read a known text file or bounded line range, or list a directory | standard and lean toolsets |
| `grep` | Search paths or file contents with a regex and optional glob | standard and lean toolsets |
| `edit_file` | Create or replace a file with `content`, or apply ordered exact `edits` atomically | standard toolset; mutating |
| `attach` | Add a local image or document to the next model request | when attachments are enabled |
| `show_image` | Render a local image with the terminal's native inline protocol | interactive terminals with inline-image support |
| `run` | Execute a supervised shell command, optionally yielding, using a PTY, or detaching | execute capability |
| `scratch` | Create or rerun one bounded uv-backed scratch script | standard toolset with execute capability |
| `memory` | List, search, read, or explicitly mutate native memory; automatic changes produce private audit receipts | standard toolset when memory and policy allow it |
| `web_fetch` | Read one http(s) URL as text, converting markup to what a reader would see | standard toolset; approval required |

Filesystem and external-read approval follows the active path policy. Mutating
and process tools require approval unless yolo mode is active. Child processes
receive the sanitized environment described in [SECURITY.md](../SECURITY.md).

## Activity tools

These tools are advertised when supervised background work makes them useful:

| Tool | Purpose |
| --- | --- |
| `activity` | List activities, drain or wait for bounded output, write raw characters to a retained PTY, interrupt or resize it |
| `activity_stop` | Terminate an owned process group and clean its activity state and logs |

`run` waits up to `UAGENT_RUN_YIELD_MS` (10 seconds by default) before a
still-running command becomes an activity. Explicit `yield_ms=0` waits to the
turn deadline; values from 250 through 30,000 override the initial wait. Set
`tty=true` only when the process needs interactive input.
`activity` with empty `chars` polls for up to five seconds by default; `\u0003`
interrupts the process group. PTY resize requires `rows` and `cols` together.
Normal input to a non-TTY activity is rejected. `run` and `activity` accept `max_output_chars` to lower the
host-capped output budget for one interaction.

Supervised PTY and non-TTY outputs share one event-driven process-I/O layer.
Once returned by `run` or `activity`, output is not returned again as new output. A 1 MiB head/tail
buffer preserves the oldest and newest bytes and reports an omitted middle.
Persistent detached commands remain log-based and cannot be interactively
reattached after the harness exits. Waiting readers use native kqueue/inotify
file notifications where available.

## Conditional and extensible tools

| Tool | Condition |
| --- | --- |
| `web_search` | a supported server or configured search route is available |
| `subagent` | delegation is enabled and the current depth is below its limit |
| `advisor` | a model route is set with `--advisor` or `UAGENT_ADVISOR_MODEL` |
| `skill` | at least one installed skill remains usable after tool-requirement filtering |
| `adapt_system` | `UAGENT_ADAPT_SYSTEM=1` |
| `<server>_<tool>` | discovered from a configured MCP server; names are sanitized and collision-safe |

The advisor receives workspace inspection and read-only external tools. It
cannot edit files, execute commands, use memory, or delegate another child.

`web_fetch` needs no hosted route, so it does not follow `web_search`'s
availability. It returns text only: it decodes markup, JSON, XML and plain
text, and refuses anything else rather than handing over bytes. A page behind a
login or assembled by scripting belongs to the browser skill. Bodies larger
than `UAGENT_WEB_FETCH_BYTES` are read up to the cap and marked partial.

`web_search` is always one named model-facing function. Its host implementation
selects OpenAI Responses or OpenRouter server search, so models, yolo mode, and
delegated workers receive the same schema and accounting.

Independent parallel-safe calls may execute concurrently, but results are
appended to the conversation in model call order. Tool-specific limits,
timeouts, visibility, stable-argument checks, and the global turn budget remain
host-enforced even in yolo mode.
