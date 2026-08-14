# Tools

µAgent builds the model-visible tool registry at startup and refreshes it when
MCP capabilities change. The exact set is filtered by approval policy, lean
mode, route capabilities, runtime state, installed skills, delegation depth,
and trusted configuration. `/context` prints the schemas advertised for the
next request.

## Core tools

| Tool | Purpose | Availability |
| --- | --- | --- |
| `read_file` | Read a known text file or bounded line range | standard and lean toolsets |
| `list_dir` | Inspect one directory | standard and lean toolsets |
| `grep` | Search paths or file contents with a regex and optional glob | standard and lean toolsets |
| `write_file` | Create or completely replace a file | standard toolset; mutating |
| `edit_file` | Apply one or more exact replacements atomically | standard toolset; mutating |
| `attach` | Add a local image or document to the next model request | when attachments are enabled |
| `show_image` | Render a local image with the terminal's native inline protocol | interactive terminals with inline-image support |
| `run` | Execute a supervised shell command, optionally yielding, using a PTY, or detaching | execute capability |
| `run_python` | Create or rerun one bounded uv-backed scratch script | standard toolset with execute capability |
| `memory` | List, search, read, or explicitly mutate native memory | standard toolset when memory and policy allow it |

Filesystem and external-read approval follows the active path policy. Mutating
and process tools require approval unless yolo mode is active. Child processes
receive the sanitized environment described in [SECURITY.md](../SECURITY.md).

## Activity tools

These tools are advertised when supervised background work makes them useful:

| Tool | Purpose |
| --- | --- |
| `activity_output` | List activities, drain new bounded output, or wait for output, exit, or a readiness marker |
| `activity_input` | Write raw characters to a retained PTY, poll it, interrupt a process group, or resize the PTY |
| `activity_wait` | Join any or all selected non-detached activities when the next step requires their results |
| `activity_stop` | Terminate an owned process group and clean its activity state and logs |

`run` waits up to `UAGENT_RUN_YIELD_MS` (10 seconds by default) before a
still-running command becomes an activity. Explicit `yield_ms=0` waits to the
turn deadline; values from 250 through 30,000 override the initial wait. Set
`tty=true` only when the process needs interactive input.
`activity_input.chars` polls for up to five seconds by default; `\u0003`
interrupts the process group. PTY resize requires `rows` and `cols` together.
Normal input to a non-TTY activity is rejected. `run`, `activity_output`,
`activity_input`, and `activity_wait` accept `max_output_chars` to lower the
host-capped output budget for one interaction.

Supervised PTY and non-TTY outputs share one event-driven process-I/O layer.
Once returned by `run`, `activity_output`, or `activity_input`, output is not returned again as new output. A 1 MiB head/tail
buffer preserves the oldest and newest bytes and reports an omitted middle.
Persistent detached commands remain log-based and cannot be interactively
reattached after the harness exits.

## Conditional and extensible tools

| Tool | Condition |
| --- | --- |
| `web_search` | a supported server or configured search route is available |
| `task` | delegation is enabled and the current depth is below its limit |
| `skill` | at least one installed skill remains usable after tool-requirement filtering |
| `adapt_system` | `UAGENT_ADAPT_SYSTEM=1` |
| `<server>_<tool>` | discovered from a configured MCP server; names are sanitized and collision-safe |

Independent parallel-safe calls may execute concurrently, but results are
appended to the conversation in model call order. Tool-specific limits,
timeouts, visibility, stable-argument checks, and the global turn budget remain
host-enforced even in yolo mode.
