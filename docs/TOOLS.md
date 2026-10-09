# Tools

The tools µAgent offers the model, when each is available, and how calls are
approved.

| To | Use |
| --- | --- |
| see what this conversation offers, MCP tools included | `/context` |
| switch a tool off or on for this conversation | `/tools off NAME`, `/tools on NAME`, `/tools reset` |
| narrow the set in one step | `/tools profile coding\|research\|minimal\|default` |
| limit every session to kinds of tools | `UAGENT_TOOL_CAPABILITIES`, a comma-separated list of `inspect`, `execute`, `mutate`, `delegate`, `external` |
| choose how calls are approved | `/permissions ask\|auto\|yolo`; see [Approval](#approval) |

The exact schemas and their sizes are in the generated
[tool reference](../skills/uagent-config/references/tools.md).

## Inventory

The set is built when a session starts and refreshed when an MCP server's
tool list changes. "Full toolset" means every session except a `lean`
subagent, which reads and runs but gets no file-editing, memory, `uagent`
or `subagent` tool.

| Tool | Purpose | Available |
| --- | --- | --- |
| `read_path` | read text or line ranges, list directories, load images and documents | always |
| `grep` | regex or literal search of file contents or paths | always |
| `write_file` | create a file; replacing one needs `overwrite=true` | full toolset |
| `edit_file` | apply ordered exact replacements atomically | full toolset |
| `delete_file` | delete a regular file and show the removed content | full toolset |
| `run` | run a supervised shell command, optionally with a PTY or detached | always |
| `scratch` | run a `.py` (under uv) or `.sh` script in `.uagent/scratch`, given as `code` in the call or written before with `write_file`; writes there need no approval, the run does and shows the script | `uv` or `python3` on `PATH` |
| `activity` | list, poll, wait for, write to, resize or stop activities | once a command is still running or detached |
| `memory` | list, search and read memory; write when the user asks | full toolset, memory enabled |
| `uagent` | inspect this build (status, flags, commands, config, tools, prompt, routes, instructions); change settings and instruction files | full toolset; changes only where a person can approve |
| `web_fetch` | read one public http(s) URL as text | always |
| `artifact` | hand the user a file to open or download (HTML runs sandboxed, PDFs and images open inline); snapshot into the session's assets | a session with a client |
| `web_search` | cited web search through OpenRouter's hosted search | an OpenRouter-protocol route or search endpoint |
| `thread`, `decide`, `state`, `history` | a coordinator's own: start, guide, stop and remove threads and chat members; answer what a thread asks or pass it to you; keep pinned notes (goals, decisions, open questions); search and read the folder's conversations | a folder's coordinator only; see the [guide](GUIDE.md#the-coordinator) |
| `session` | list linked sessions and message them; an idle one starts a turn on the message | always; a coordinator and its threads are linked, and so are YOLO sessions in one folder |
| `ask` | put 1 to 8 multiple-choice questions to the user and wait; an option can show an image the agent made in the workspace and a monospace preview; they may answer in their own words or with an image | a session someone can answer (never headless runs or children); a thread's questions go to its coordinator first |
| `subagent` | delegate a subtask to a durable child session | full toolset, delegation depth below `UAGENT_SUBAGENT_DEPTH` |
| `skill` | load an installed skill | a usable skill is installed |
| `adapt_system` | add to or replace the base prompt for this conversation | `UAGENT_ADAPT_SYSTEM=1`; see [INSTRUCTIONS.md](INSTRUCTIONS.md) |
| `browser` | drive the shared Chrome of the browser appliance | top-level sessions (web, terminal, headless, a coordinator's threads) while the web host's browser runs and `UAGENT_BROWSER_DATA` names it; one conversation at a time; see [BROWSER.md](BROWSER.md) |
| `<server>_<tool>` | tools discovered from MCP servers; see [OPERATIONS.md](OPERATIONS.md#mcp) | configured servers; not in lean children |

Independent calls to parallel-safe tools run up to four at a time; results
are appended in the order the model issued the calls. A tool with a per-turn
cap (`web_search` 4, `subagent` 32) is withdrawn for the rest of the turn once
the cap is reached. Tool limits, timeouts and turn budgets apply in every
approval mode.

## Approval

Reading and searching inside the workspace needs no approval. Calls that
change files, run commands, use the network or touch a path outside the
workspace follow the approval mode:

| To | Use |
| --- | --- |
| approve each call yourself (the default) | `/permissions ask` |
| let a reviewer model decide | `/permissions auto`; needs `OPENROUTER_API_KEY`, and `UAGENT_PERMISSION_MODEL` and `UAGENT_PERMISSION_URL` choose the reviewer |
| run them unasked, commands still sandboxed | `/permissions yolo`, `/yolo` or `--yolo` |
| save a mode as the default | `UAGENT_APPROVAL` |
| list or remove remembered approvals | `/permissions rules`, `/permissions forget N\|all` |

What each mode covers, what always needs a person, and how subagents and a
coordinator's threads differ is in
[SECURITY.md](../SECURITY.md#what-the-agent-may-do-without-asking).

## Files and search

- `read_path` loads images, documents and binary files through the same
  capability-aware pipeline as attachments; omit line ranges for media.
- `write_file` publishes atomically. Without `overwrite=true` it refuses any
  existing path, including a symlink; use `edit_file` for targeted changes.
- `grep` defaults to regex content matches. `literal=true` searches exact
  text, `mode=files` matches file names and `mode=matching_files` returns the
  paths whose contents match; both path modes ignore `context`.
- `/changes` lists the files the last turn's `write_file`, `edit_file` and
  `delete_file` calls changed, and `/undo [FILE]` puts them back. Changes
  made by shell commands are not tracked.
- `run`, `scratch` and `grep` execute inside the OS sandbox: writes are
  limited to the workspace, temporary directories and package caches; reads
  and, by default, the network stay open, except for the browser profile,
  which the file tools refuse too. See [SECURITY.md](../SECURITY.md) for the
  full policy.

## Activities

`run` waits 10 s and then returns a still-running command as an activity.
`yield_ms` of 250–30,000 overrides the wait and `0` waits until the command
exits. Set `tty=true` only when the process needs interactive input; a PTY
keeps merged output, writable input, interruption and resize. `detach=true`
keeps the command running after the session ends, with a rotating log.

Every `activity` call names one `operation`:

| Operation | Arguments |
| --- | --- |
| `list` | |
| `poll` | `id`; optional `wait_ms` and `until` (a readiness marker) |
| `wait` | `wait_ms`; optional `ids` (default: all) and `mode=any\|all` |
| `write` | `id`, `chars`; an empty string is a valid write and `\u0003` interrupts the process group |
| `resize` | `id`, `rows` and `cols` in 1..1000; PTY activities only |
| `stop` | `id`; ends the process group and removes its records and logs |

`run` and `activity` accept `max_output_chars` (256–65,536) to lower the output
cap for one call. Output returned once is not returned again. Writing to a
non-PTY activity is rejected.

- Activities of a conversation live only in memory. Detached ones are
  PID-backed with a rotating log; launching the same detached command from
  the same directory reuses its process group, and it cannot be reattached
  interactively after µAgent exits.
- A command's completion appears only in the UI and never starts a model
  turn. A subagent's completion is added once to the next model call without
  starting one.
- `stop` sends TERM, then KILL, to the whole process group and removes its
  records and logs.
- A failed child reports its route, failure stage, bounded diagnostics and a
  remedy. µAgent never silently changes provider, model, pricing or privacy
  policy for a child.

## Delegation

`subagent` defaults to `operation=spawn` and waits for the child, returning
its answer and a durable agent ID; with `background=true` it returns an
activity ID at once.

- `mode` is `lean` by default (read and run, no file-editing tools); `full`
  adds the editing tools and lets the child delegate in turn. A child never
  gets a tool its parent has switched off.
- `model` picks the child's route; without it the child runs on the
  parent's.
- `limits` lowers or raises `steps`, `tool_calls`, `seconds` and `cost` for
  one child, within the session's remaining budgets; `memory=false` withholds
  memory.
- `followup` resumes the child's private conversation and prepends its
  stored `directive`; a new directive replaces it.
- `message` delivers one-shot guidance at a running child's next step; a
  finished child runs again on it.
- `list` reports this session's children with model, toolset and state.
- Use `activity` to wait for, read or stop ordinary children. `/agents` shows
  the same records.

## Web

`web_fetch` needs no hosted-search route. It decodes HTML, JSON, XML and plain
text and refuses other content types; download PDFs and images with `run` and
open them with `read_path`. Bodies over the attachment cap
(`UAGENT_ATTACHMENT_MB`) are truncated and marked partial. Pages behind a login
or built by scripts need the browser.

Only public Internet addresses are accepted, on redirects too; loopback,
private and link-local addresses are refused and proxy variables are
ignored.

`web_search` calls OpenRouter's hosted search on its own route, whatever the
conversation model. `UAGENT_WEB_SEARCH_MODEL` names that route and
`UAGENT_WEB_SEARCH_BACKEND=off` withholds the tool.

`browser` drives the web appliance's shared Chrome; [BROWSER.md](BROWSER.md)
explains setup, saved logins and taking over.

## Presentation

Adjacent successful calls of one kind fold into one row: Explored,
Researched, Verified or Edited. A failure or an approval prompt keeps its
own row.

How much of this is shown is one display setting, `UAGENT_VERBOSITY`, read by
the terminal and the web alike and never by the model:

| Level | Tool work | Arguments and output | Thinking | Routine notices |
| --- | --- | --- | --- | --- |
| `minimal` | one "Worked · N steps" row per turn | inside that row | hidden | hidden |
| `default` | the groups above, other calls a row each | on request | closed (web), hidden (terminal) | hidden |
| `full` | every call its own row | shown | shown | shown |

`/verbosity LEVEL` (or Detail in a web conversation's menu) changes it for
every terminal and browser, across all conversations; `--verbosity LEVEL`
pins one terminal for one run.
