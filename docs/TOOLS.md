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

The exact schemas and their sizes come from the `uagent` tool with topic
`tools`. An action that carries its own payload is a tool of its own, so its
schema requires those fields; a tool with several actions lists the fields
each one needs in its description. A call missing one does not run: the
model is told every missing field and what the call carried, and the third
such round in a turn ends it.

## Inventory

The set is built when a session starts and refreshed when an MCP server's
tool list changes. "Full toolset" means every session except an agent
started with `edits` none, which gets no `write_file`, `edit_file`,
`delete_file`, `memory` or `browser` tool.

| Tool | Purpose | Available |
| --- | --- | --- |
| `read_path` | read text or line ranges, list directories, load images and documents | always |
| `grep` | regex or literal search of file contents or paths | always |
| `write_file` | create a file; replacing one needs `overwrite=true` | full toolset |
| `edit_file` | apply ordered exact replacements atomically | full toolset |
| `delete_file` | delete a regular file and show the removed content | full toolset |
| `run` | run a supervised shell command, optionally with a PTY or detached | always |
| `scratch` | write and run a one-off script in `.uagent/scratch`: `.py` under uv (or `python3` when it declares no dependencies), `.sh` under `sh`, `.js`/`.mjs` under `node`; writing the script needs no approval, running it does and shows the script; it runs in the workspace, and the result names what it left in the workspace root | `uv`, `python3`, `node` or `sh` on `PATH` |
| `activity` | list, poll, wait for, write to, resize or stop activities | always |
| `memory` | list, search and read memory; write when the user asks | full toolset, memory enabled |
| `uagent` | inspect this build (status, flags, commands, config, tools, prompt, routes, instructions) | always |
| `uagent_configure`, `uagent_instructions` | change settings, or replace an instructions file, by an exact diff a person approves | only where a person can approve |
| `web_fetch` | read one public http(s) URL as text | always |
| `artifact` | hand the user a file to open or download (HTML runs sandboxed, PDFs and images open inline), snapshotted into the session's assets | a session with a client |
| `web_search` | cited web search through a search plugin an endpoint runs (OpenRouter's) | a search can run on the conversation's route (see below) |
| `todo` | replace the conversation's to-do list (up to 24 items, each pending, in progress or done, at most one in progress); it is kept with the conversation and carried on every request while it has items | always |
| `agent_start` | start agents, each a conversation of its own on a brief; see [Agents](#agents) | unless this agent is `agents.depth` levels deep |
| `agent_send`, `agents`, `decide` | message the parent, an agent or its siblings; list, read, stop, close or delete agents; answer what an agent asks | always |
| `ask` | put 1 to 8 multiple-choice questions to the user and wait; an option can show an image the agent made in the folder and a monospace preview; the user may answer in words or with an image | a session someone can answer (never a headless run); an agent's questions go to its parent first |
| `skill` | load an installed skill | a usable skill is installed |
| `browser` | drive the web host's shared Chrome; access queues, and isolated parallel research is opt-in | full toolset, while the web host runs the browser named by `UAGENT_BROWSER_DATA`; see [BROWSER.md](BROWSER.md) |
| `<server>_<tool>` | tools discovered from MCP servers; see [Operations](OPERATIONS.md#mcp) | configured servers; not in a lead agent or an agent |

A result is its output, which the model reads and you see; facts (an exit
code, lines added and removed, a count, why a call was refused), which every
row words the same way, such as `exit 2 · 1.4s` or `+3 −1`; and a hint for
the model only, such as a read's line range, a command's exit code or where a
captured log is kept, which it reads after the output and no row shows.

Independent calls to parallel-safe tools run up to four at a time; results
are appended in the order the model issued the calls. A tool with a per-turn
cap (`web_search` 4) is withdrawn for the rest of the turn
once the cap is reached. Tool limits, timeouts and turn budgets apply in
every approval mode.

## Approval

Reading and searching inside the folder needs no approval. Calls that
change files, run commands, use the network or touch a path outside the
folder follow the approval mode: `ask` (the default), `auto` or `yolo`
(also `/yolo` or `--yolo`), set with `/permissions` and saved as the default
with `UAGENT_APPROVAL`. Reading one of your other folders, such as another
repository in the home folder, always asks a person (`sandbox.read` opens a
folder), except under yolo, which opens them. The
[guide](GUIDE.md#approvals-and-the-sandbox) explains the modes; none of them
turns the sandbox off.

`/permissions rules` lists remembered approvals and
`/permissions forget N|all` removes them.
[Security](../SECURITY.md#what-the-agent-may-do-without-asking) says what
each mode covers and what always needs a person.

An agent takes its parent's mode, Auto at most: Ask under a parent in Ask,
Auto otherwise. A person may set any mode for it, yolo included, and that
survives a restart. In Auto, an agent that may edit acts inside its folder
without review while the sandbox is enforced. Its approvals and questions go
to its parent, which answers with `decide` (allow once or for the agent's
conversation, deny with a reason, answer, or yield). One the parent yields
or leaves for 5 minutes goes to a person, and so does one that always needs
a person, such as a command outside the sandbox or `agents` `delete`. Once a
person writes into an agent's turn, the rest of that turn's decisions are
theirs.

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
- `run`, `scratch` and `grep` run inside the OS sandbox: writes are limited
  to the folder, temporary directories and package caches, and the hidden
  stores (the browser profile, µAgent's settings and other tools' credential
  stores) cannot be read; the file tools refuse them too.
  [Sandboxing](../SECURITY.md#sandboxing) has the full policy and the
  settings that change it.

## Activities

`run` waits 10 s and then returns a still-running command as an activity.
`yield_ms` of 250–30,000 overrides the wait and `0` waits until the command
exits. Guidance sent meanwhile moves the command to the background, as it
ends an `activity` wait. Set `tty=true` only when the process needs
interactive input; a PTY keeps merged output, writable input, interruption
and resize. `detach=true` keeps the command running after the session ends,
with a rotating log.

Every `activity` call names one `operation`:

| Operation | Arguments |
| --- | --- |
| `list` | |
| `poll` | `id`; optional `wait_ms` and `until` (a readiness marker) |
| `wait` | `wait_ms`; optional `ids` (default: all) and `mode=any\|all` |
| `write` | `id`; optional `chars`: left out or empty is a valid write, and `\u0003` interrupts the process group |
| `resize` | `id`, `rows` and `cols` in 1..1000; PTY activities only |
| `stop` | `id`; sends TERM, then KILL, to the process group and removes its records and logs |

`run` and `activity` accept `max_output_chars` (256–65,536) to lower the
output cap for one call. Output returned once is not returned again. Writing
to a non-PTY activity is rejected.

- A conversation's activities live only in memory. Detached ones are
  PID-backed with a rotating log; launching the same detached command from
  the same directory reuses its process group, and it cannot be reattached
  interactively after µAgent exits.
- An activity's result is added once, bounded, to
  the model's next call; output already read is not repeated. If the
  conversation is idle by then, the result starts a turn, so the agent can
  end its turn instead of waiting. A headless run (`-p`) starts no turn for
  a late command result, and its commands stop when it ends.

## Agents

An agent is a conversation another one, its parent, started with
`agent_start`. It is saved beside its parent, runs in its own runtime, opens
like any conversation and is listed under its parent; `-c` and `--resume`
skip it. With no client attached, its runtime stops about a second after its
turn ends, and a message starts it again.

`agent_start` takes a `brief` the agent must be able to finish alone.

- `edits` is `none` by default: the agent's commands run in the `readonly`
  sandbox and it gets no editing tools, whatever its conversation's settings
  say; the agents it starts are `none` too. `worktree` works in a new git
  worktree under `~/.uagent/worktrees/`, `folder` in this folder; both run in
  the `default` sandbox.
- `model` picks its route; without it, the parent's.
- With `wait` (the default) the call returns `[{id, state, answer}]` once
  every agent has reported, or as soon as one asks for a decision (state
  `needs decision`). Esc stops them; the turn's deadline leaves them
  working. Without `wait`, the agent's report (its answer, up to 6,000
  characters) arrives as mail and starts the parent's next turn.
- `items` (up to 24) starts one agent per item, the item appended to the
  brief, and adds `item` to each row.
- A conversation has at most `agents.max` (8) agents at work; items beyond
  the free slots start as slots free. Agents start agents down to
  `agents.depth` (2) levels, and start no MCP servers.

A headless run waits while its agents owe it a report, except one waiting on
a person, and takes each up as a turn.

`agent_send` messages the parent, an agent this conversation started or one
of its siblings: a running one reads it at its next step, an idle one starts
a turn on it. Agents' mail starts at most three turns in a row of one
conversation before a person speaks there.

`agents` lists this conversation's agents; reads any conversation of the
folder (`part` `rows`, `report`, `diff`, or `search` with `query`), quoted as
data; and stops, closes or deletes an agent of its own. Deleting asks a
person and removes the agent's worktree only when nothing in it would be
lost.

## Web

`web_fetch` needs no hosted-search route. It decodes HTML, JSON, XML and
plain text. A PDF is saved and attached to the model's next step, as
`read_path` attaches one: a route that reads documents gets it whole, any
other its path. Other content types are refused; download images with `run`
and open them with `read_path`. A 404 says to find the page with
`web_search` rather than guess its address. Bodies over the attachment cap
(`UAGENT_ATTACHMENT_MB`) are truncated and marked partial. Pages behind a
login or built by scripts need the browser. Only public Internet addresses
are accepted, on redirects too; loopback, private and link-local addresses
are refused and proxy variables are ignored.

`web_search` calls the search plugin of an endpoint that declares one
(OpenRouter does) on a route of its own: the conversation's when it declares
one, else the first configured endpoint that does, with the model
`UAGENT_WEB_SEARCH_MODEL` names. A route with hosted search of its own uses
that instead. The tool is offered only while one of these can run on the
route in use, so switching models can add or withdraw it; when an endpoint
offers search but no model is named for it, you are told once what to set.
`UAGENT_WEB_SEARCH_BACKEND=off` withholds it.

`browser` drives the web host's shared Chrome; [BROWSER.md](BROWSER.md)
covers setup, saved logins and taking over. Its `open` with `path` shows a
self-contained `.html` or `.svg` file the agent wrote, without a server.

## Presentation

The host turns each conversation into one tree of nodes
([`node.h`](../include/agent/node.h)): a turn holds your message, a work
node, the answer and its footer; guidance read while it runs is a message
of the turn, followed by the work replying to it. A work node holds the
interim replies, thinking, groups and calls. Every row has the same shape:
a headline (verb and target, e.g. "Ran npm test"), a result word from the
call's facts (exit code, lines added and removed, a count, or why it
stopped), a state (running, ok, failed, cancelled) and a body to open.
Nothing in it names a tool, provider or model; an MCP or unknown tool reads
"Called NAME".

Adjacent successful calls of one kind fold into one group (Explored,
Researched, Verified or Edited). A failure or an approval prompt keeps its
own row. The work line counts what it holds: "Worked 6 steps · edited 1
file · 1 error".

The terminal and the web draw the same tree. The detail level,
`UAGENT_DETAIL`, only sets how deep it starts open, and the model never
reads it:

| Level | Depth | Shows |
| --- | --- | --- |
| `minimal` | 0 | each turn's work as one line (the current step while it runs) |
| `default` | 1 | the work's steps, each one closed line |
| `full` | all | every row open, routine notices included |

`/detail LEVEL` changes it for every terminal, and a browser starts at it;
Detail in a web conversation's menu sets that browser's own level.
`--detail LEVEL` pins one terminal for one run.
