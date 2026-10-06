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
| `scratch` | run a `.py` (under uv) or `.sh` script written to `.uagent/scratch` with `write_file`; writes there need no approval, the run does and shows the script | `uv` or `python3` on `PATH` |
| `activity` | list, poll, wait for, write to, resize or stop activities | once a command is still running or detached |
| `memory` | list, search and read memory; write when the user asks | full toolset, memory enabled |
| `uagent` | inspect this build (status, flags, commands, config, tools, prompt, routes, instructions); change settings and instruction files | full toolset; changes only where a person can approve |
| `web_fetch` | read one public http(s) URL as text | always |
| `artifact` | hand the user a file to open or download (HTML runs sandboxed, PDFs and images open inline); snapshot into the session's assets | a session with a client |
| `web_search` | cited web search through OpenRouter's hosted search | an OpenRouter-protocol route or search endpoint |
| `session` | list linked sessions and message them; an idle one starts a turn on the message | always; a coordinator and its threads are linked, and so are YOLO sessions in one folder |
| `ask` | put 1 to 8 multiple-choice questions to the user and wait; an option can show an image the agent made in the workspace and a monospace preview; they may answer in their own words or with an image | a session someone can answer (never headless runs or children); a thread's questions go to its coordinator first |
| `subagent` | delegate a subtask to a durable child session | full toolset, delegation depth below `UAGENT_SUBAGENT_DEPTH` |
| `skill` | load an installed skill | a usable skill is installed |
| `adapt_system` | add to or replace the base prompt for this conversation | `UAGENT_ADAPT_SYSTEM=1`; see [SYSTEM_PROMPTS.md](SYSTEM_PROMPTS.md) |
| `browser` | drive the shared Chrome of the browser appliance | top-level sessions (web, terminal, headless, a coordinator's threads) while the web host's browser runs and `UAGENT_BROWSER_DATA` names it; one conversation at a time; see [WEB.md](WEB.md) |
| `<server>_<tool>` | tools discovered from MCP servers; see [OPERATIONS.md](OPERATIONS.md#mcp) | configured servers; not in lean children |

Independent calls to parallel-safe tools run up to four at a time; results
are appended in the order the model issued the calls. A tool with a per-turn
cap (`web_search` 4, `subagent` 32) is withdrawn for the rest of the turn once
the cap is reached. Tool limits, timeouts and turn budgets apply in every
approval mode.

## Approval

Reading and searching inside the workspace needs no approval. Calls that
change files, run commands, use the network or touch a path outside the
workspace follow the approval mode. Set it with `/permissions ask|auto|yolo`
for a conversation, `--yolo` or `/yolo` as shortcuts, or `UAGENT_APPROVAL` as
the saved default (`ask`).

- **Ask** shows the full action and its risks (runs commands, makes changes,
  uses the network, outside this folder). Allow it once, for the session, or
  always for that exact action in this repository. A remembered rule covers
  the tool's provider, schema, approval class and arguments, so a change to
  any of them asks again. Rules live in `~/.uagent/config/permissions.json`;
  list them with `/permissions rules` and remove them with
  `/permissions forget N|all` or in the web's Settings.
- **Auto** sends the user request and a bounded preview of the action to
  OpenRouter's Decisions API (`UAGENT_PERMISSION_MODEL`, default
  `~typesafe/jev-latest`; `UAGENT_PERMISSION_URL`) and follows its allow, ask
  or deny answer. It needs `OPENROUTER_API_KEY`. An ask opens the normal
  prompt, or denies when no interactive client is attached; network,
  authentication and parse failures are treated the same way. Reviewer usage
  counts toward the turn and session.
- **YOLO** approves these calls without asking. It does not turn the command
  sandbox off: that is its own setting (`UAGENT_SANDBOX`), the same in every
  mode.

Two kinds of session differ:

- A subagent approves its own calls, so approving the delegation approves
  what the child then does. Its commands run under the sandbox of the
  conversation that delegated to it.
- A coordinator's thread runs in Auto unless it is told to ask, and cannot be
  put in YOLO. While the sandbox is enforced, its commands and its file
  changes inside the folder run without review.

Some actions always need a person, in every mode:

- reading or writing saved settings (`~/.uagent/config/settings.json`), a
  legacy `.config` file, `.mcp.json` or `permissions.json`;
- writing the project trust store or your instruction files in `~/.uagent`;
- changing settings or instruction files through `uagent`;
- replacing the base prompt with `adapt_system`;
- `run(sandbox=false)`, which runs one command outside the sandbox.

Remembered rules, Auto and YOLO do not apply to these, and a session with
nobody to ask denies them. Child processes get the sanitized environment
described in [SECURITY.md](../SECURITY.md).

## Files and search

- `read_path` loads images, documents and binary files through the same
  capability-aware pipeline as attachments; omit line ranges for media.
- `write_file` publishes atomically. Without `overwrite=true` it refuses any
  existing path, including a symlink; use `edit_file` for targeted changes.
- `grep` defaults to regex content matches. `literal=true` searches exact
  text, `mode=files` matches file names and `mode=matching_files` returns the
  paths whose contents match; both path modes ignore `context`.
- A byte-identical repeat of a `read_path` or `grep` result still in recent
  context is returned as a short receipt.
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

## Delegation

`subagent` defaults to `operation=spawn` and waits for the child, returning
its answer and a durable agent ID; with `background=true` it returns an
activity ID at once.

- `mode` is `lean` by default (read and run, no file edits); `full` adds the
  editing tools and lets the child delegate in turn. A child never gets a
  tool its parent has switched off.
- `model` picks the child's route; without it `UAGENT_SUBAGENT_MODEL`
  applies, else the parent's route.
- `limits` lowers or raises `steps`, `tool_calls`, `seconds` and `cost` for
  one child, within the session's remaining budgets; `memory=false` withholds
  memory.
- `followup` resumes the child's private conversation and prepends its
  stored `directive`; an empty directive clears it.
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

Only public Internet addresses are accepted. Every resolved IPv4 and IPv6
address of the initial request and of each redirect is checked; loopback,
private, carrier-grade NAT, link-local, reserved and multicast addresses are
refused, including IPv4-mapped, NAT64, Teredo and 6to4 forms. Proxy variables
are ignored so a proxy cannot resolve an unchecked address.

`web_search` has one schema for every model. It calls OpenRouter's hosted
`openrouter:web_search` on its own route, so citations and accounting do not
depend on the conversation model. `UAGENT_WEB_SEARCH_MODEL` names that route
and `UAGENT_WEB_SEARCH_BACKEND=off` withholds the tool.

`browser` drives the web appliance's shared Chrome, which you can watch and
take over. Each action returns a fresh screenshot and the page text. Chrome
keeps your saved logins: the agent has Chrome fill one (`fill_saved`) and
never types a password. For a login Chrome has not saved, MFA, a captcha or a
payment confirmation it calls `request_human` and waits for you. Actions
that change the page (open, click, type, scroll) follow the approval mode;
looking does not. See [WEB.md](WEB.md) for setup and hand-over. Outside the
appliance, the `browser-use` skill drives `playwright-cli` through `run`.

## Presentation

Each call carries an intent: `explore` (read, list, search), `research`
(web search, fetch, browser), `edit`, `verify` (test, lint, build), `run`,
`setup` (install, configure) or `delegate`; memory and shared files keep their
own rows. Native tools know theirs. `run` and `scratch` take an optional
`intent` from the model; without one, a command made only of read-only
programs (`ls`, `cat`, `rg`, `git log`, …) reads as `explore`, anything else
as `run`. Intent labels and groups calls and never changes permissions.

Adjacent successful calls of one intent fold into one row: Explored,
Researched, Verified or Edited, in the web UI and as a compact terminal
summary. A failure or an approval prompt keeps its own row.

How much of this is shown is one display setting, `UAGENT_VERBOSITY`, read by
the terminal and the web alike and never by the model:

| Level | Tool work | Arguments and output | Thinking | Routine notices |
| --- | --- | --- | --- | --- |
| `minimal` | one "Worked · N steps" row per turn | inside that row | hidden | hidden |
| `default` | the groups above, other calls a row each | on request | closed (web), hidden (terminal) | hidden |
| `full` | every call its own row | shown | shown | shown |

`/verbosity LEVEL` (or Detail in a web conversation's menu) changes it for
every terminal and browser, and what is already on screen is shown again at
the new level: browsers restyle at once, a terminal clears and replays the
conversation (a terminal attached elsewhere follows at its next turn; plain
and piped output changes from the next row on). It is one level for all
conversations; `--verbosity LEVEL` pins one terminal for one run.

## How a call reads

Each tool declares how its row reads, as data both clients render the same
way (`ToolView` in `include/tools/tool.h`):

- A headline of verb and target: "Editing src/a.ts" while it runs, "Edited
  src/a.ts" after. Tools without their own verbs read "Called <tool>".
- Visible without expanding: a change's diff, a command's output, and the
  parts the call produced. A shared file previews inline
  (images, sandboxed HTML, PDF on desktop) with Open and Download; started
  work links to its agent, activity or memory.
- On expand: the call's input (a command, code or fields) and its full
  output.

Commands keep their output in one scrollable box under the row, opened at the
end. Memory saves and finished background work use the same row.
