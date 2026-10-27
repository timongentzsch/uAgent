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
subagent, which reads and runs but gets no file-editing, memory, `uagent` or
`subagent` tool.

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
| `uagent` | inspect this build (status, flags, commands, config, tools, prompt, routes, instructions); change settings and instruction files | full toolset; changes only where a person can approve |
| `web_fetch` | read one public http(s) URL as text | always |
| `artifact` | hand the user a file to open or download (HTML runs sandboxed, PDFs and images open inline), snapshotted into the session's assets | a session with a client |
| `web_search` | cited web search through a search plugin an endpoint runs (OpenRouter's) | a search can run on the conversation's route (see below) |
| `thread`, `decide`, `plan`, `history` | the lead agent's own (it also has `run`, in the `readonly` sandbox): start, guide, stop and remove background agents (`thread`); answer what a background agent asks or pass it to you; keep the plan (goals, decisions, open questions, and steps with their status and result); search and read the folder's conversations | a folder's lead agent, and a `/btw` side conversation started from one; see the [guide](GUIDE.md#the-lead-agent) |
| `session` | list linked sessions and message them; an idle one starts a turn on the message | always; a lead agent and its background agents are linked, and so are YOLO sessions in one folder |
| `ask` | put 1 to 8 multiple-choice questions to the user and wait; an option can show an image the agent made in the workspace and a monospace preview; the user may answer in words or with an image | a session someone can answer (never headless runs or children); a background agent's questions go to its lead agent first |
| `subagent` | delegate a subtask to a durable child session | full toolset, delegation depth below `UAGENT_SUBAGENT_DEPTH` |
| `skill` | load an installed skill | a usable skill is installed |
| `browser` | drive the web host's shared Chrome; access queues, and isolated parallel research is opt-in | top-level sessions (web, terminal, headless, lead agents and background agents) while the web host runs the browser named by `UAGENT_BROWSER_DATA`; see [BROWSER.md](BROWSER.md) |
| `<server>_<tool>` | tools discovered from MCP servers; see [Operations](OPERATIONS.md#mcp) | configured servers; not in a lead agent or a lean child |

Independent calls to parallel-safe tools run up to four at a time; results
are appended in the order the model issued the calls. A tool with a per-turn
cap (`web_search` 4, `subagent` 32) is withdrawn for the rest of the turn
once the cap is reached. Tool limits, timeouts and turn budgets apply in
every approval mode.

## Approval

Reading and searching inside the workspace needs no approval. Calls that
change files, run commands, use the network or touch a path outside the
workspace follow the approval mode: `ask` (the default), `auto` or `yolo`
(also `/yolo` or `--yolo`), set with `/permissions` and saved as the default
with `UAGENT_APPROVAL`. Reading one of your other folders, such as another
repository in the home folder, always asks a person (`sandbox.read` opens a
folder). The [guide](GUIDE.md#approvals-and-the-sandbox) explains the modes;
none of them turns the sandbox off.

`/permissions rules` lists remembered approvals and
`/permissions forget N|all` removes them.
[Security](../SECURITY.md#what-the-agent-may-do-without-asking) says what
each mode covers, what always needs a person, and how subagents and a
lead agent's background agents differ.

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
  to the workspace, temporary directories and package caches, and the hidden
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
| `write` | `id`, `chars`; an empty string is a valid write and `\u0003` interrupts the process group |
| `resize` | `id`, `rows` and `cols` in 1..1000; PTY activities only |
| `stop` | `id`; sends TERM, then KILL, to the process group and removes its records and logs |

`run` and `activity` accept `max_output_chars` (256–65,536) to lower the
output cap for one call. Output returned once is not returned again. Writing
to a non-PTY activity is rejected.

- A conversation's activities live only in memory. Detached ones are
  PID-backed with a rotating log; launching the same detached command from
  the same directory reuses its process group, and it cannot be reattached
  interactively after µAgent exits.
- A background command's or subagent's result is added once, bounded, to
  the model's next call; output already read is not repeated. If the
  conversation is idle by then, the result starts a turn, so the agent can
  end its turn instead of waiting. A headless run (`-p`) answers once: it
  waits for its subagents but starts no turn for a late result, and its
  commands stop when it ends.
- A failed child reports its route, failure stage, bounded diagnostics and a
  remedy. µAgent never silently changes provider, model, pricing or privacy
  policy for a child.

## Delegation

`subagent` defaults to `operation=spawn` and waits for the child, returning
its answer and a durable agent ID; with `background=true` it returns an
activity ID at once.

- `mode` is `lean` by default (read and run, no file-editing tools); `full`
  adds the editing tools and lets the child delegate in turn. A child never
  gets a tool its parent has switched off. In a session with no command
  tool, such as a lead agent, a lean child only reads, so starting one needs
  no approval, and the tool tells the parent its children cannot run
  commands.
- `model` picks the child's route; without it the child runs on the route
  `subagent.model` names, or on the parent's when that is empty.
- `limits` lowers or raises `steps`, `tool_calls`, `seconds` and `cost` for
  one child, within the session's remaining budgets; `limits.memory=false`
  withholds memory.
- `schema` (a JSON Schema) makes the child answer with only that JSON. The
  brief tells it the format; an answer that does not parse or fit goes back
  to it with the reason, twice at most, after which it stands as written.
- `step` names the plan step the child works for; `list` reports it.
- `fanout` runs `prompt` once per entry of `items` (put in its `{item}`),
  each child answering with findings of the `finding` schema. Then
  `refuters` children (default 1, at most 3) try to refute each finding.
  It returns the verified findings, the refuted ones with a reason, and those
  left unchecked, with no model turn in between. One call starts at most 64
  children. A finding no refuter checked, with `refuters=0` or past that
  ceiling, is reported unchecked, never verified.
- Children started in one batch run at once, up to six; other parallel
  calls run four at a time.
- `followup` resumes the child's private conversation and prepends its
  stored `directive`; a new directive replaces it.
- `message` delivers one-shot guidance at a running child's next step; a
  finished child runs again on it.
- `list` reports this session's children with model, toolset and state.
- `activity` waits for, reads or stops ordinary children. `/agents` shows the
  same records.

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

Adjacent successful calls of one kind fold into one row: Explored,
Researched, Verified or Edited. A failure or an approval prompt keeps its
own row.

How much of this is shown is one display setting, `UAGENT_VERBOSITY`, read
by the terminal and the web alike and never by the model:

| Level | Tool work | Arguments and output | Thinking | Routine notices |
| --- | --- | --- | --- | --- |
| `minimal` | one "Worked · N steps" row per turn | inside that row | hidden | hidden |
| `default` | the groups above, other calls a row each | on request | closed (web), hidden (terminal) | hidden |
| `full` | every call its own row | shown | shown | shown |

`/verbosity LEVEL` (or Detail in a web conversation's menu) changes it for
every terminal and browser, across all conversations; `--verbosity LEVEL`
pins one terminal for one run.
