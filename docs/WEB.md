# Web interface

`uagent --web` serves µAgent to a browser: the same conversations as the
terminal, on this machine or on a paired phone.

## Quick start

```sh
uagent --web
```

```text
uagent web: http://127.0.0.1:8080
Pairing code (single use, 5 minutes): <code>
Or open: http://127.0.0.1:8080/#pair=<code>
```

Open the `#pair=` link: it pairs the browser and drops the code from the
address. Or open the URL and type the code.

- **Pair another device.** Run `uagent --web` again. It reuses the running
  host, which keeps its port and origin whatever flags you pass, and prints a
  fresh code that replaces the previous one.
- **A refused code** (used, mistyped or older than five minutes) says it
  expired. Run `uagent --web` again.
- **Remove a device.** Settings → Host → Paired devices → **Revoke**, or
  **Log out this device**.

The sidebar lists this OS user's conversations by place: **Workspaces**
first, then **Folders**. A place's header is its lead agent: the name opens
it (starting one the first time), the chevron before it folds the place
away (remembered per browser) and **+** starts a conversation there. Under
the header come the background agents the lead agent started, a
divider, then the place's own conversations. Browsing saved history starts no
model and runs no command.

**New** at the top of the sidebar starts a **Workspace…** (a name) or a
**Folder…** (a directory on the host) and opens a conversation there.

- A **workspace** is a folder the host keeps under `~/.uagent/spaces/`,
  created the first time its name is used. Use one for work that belongs to
  no project, rather than the home directory, which
  [commands cannot write](../SECURITY.md#sandboxing). Its name may be several
  words; renaming it (the menu beside the name) moves nothing. Deleting a
  workspace removes its conversations and its folder with every file in it.
- A **folder** is a directory of yours. It is listed while it has
  conversations, and µAgent never removes it.

**Files** (the folder button in a conversation's header) shows the
conversation's folder beside the chat, or as a sheet on a small screen. It
never writes. Search finds a file by name anywhere in the folder. Code opens
with line numbers and highlighting, Markdown rendered with its images
(**Source** shows the text), and an image at full size on a click; any other
file offers a download. What is open is read again when a turn ends.

Drag the edge of the sidebar, the plan, Files or its list to resize it, or
focus the edge and use the arrow keys (Shift for larger steps). A
double-click or Enter restores the default width. Each device keeps its own
widths.

`--web` starts no conversation itself, so it refuses session flags (`-p`,
`--json`, `-c`, `--resume`, `--yolo`, `--attach`, `--model`, …). It reads the
settings saved for all conversations and the environment, never a project's.

| Flag | Setting | Default |
| --- | --- | --- |
| `--web-port PORT` | `UAGENT_WEB_PORT` | `8080` (1024–65535) |
| `--web-origin ORIGIN` | `UAGENT_WEB_ORIGIN` | empty (loopback only) |
| | `UAGENT_WEB_BIND` | `127.0.0.1`; `0.0.0.0` listens on all interfaces |
| | `UAGENT_WEB_PUSH_CONTACT` | empty (no background push) |
| | `UAGENT_BROWSER_DATA` | empty (no shared browser) |

Save one from a terminal session with `/config user KEY=VALUE`, or set it in
the host's environment. Each applies when the web host next starts.

## Remote access and installation

The host listens on loopback. To reach it from another device, put an HTTPS
reverse proxy in front of the port and give the host its exact public origin:

```sh
uagent --web --web-origin https://your-host.example
```

The origin is bare (scheme, host and optional port, no path). Forwarded
headers never override it, and the browser must use exactly that origin.
[Tailscale Serve](https://tailscale.com/docs/features/tailscale-serve) can
provide a private HTTPS endpoint. The proxy must forward the event stream
(`/api/events`, Server-Sent Events) and, for the
[shared browser](BROWSER.md), WebSocket upgrades on `/api/browser/viewer`.

For a quick test without HTTPS, an HTTP origin with a literal IPv4 address in
the tailnet range `100.64.0.0/10` is also accepted:

```sh
uagent --web --web-port 18080 --web-origin http://100.64.0.10:18080
tailscale serve --bg --tcp=18080 tcp://127.0.0.1:18080
```

Tailnet HTTP is not a browser secure context, so the app cannot be installed
and notifications stay off. To test those without trusted HTTPS, forward the
port and open the loopback URL:

```sh
ssh -N -L 18080:127.0.0.1:18080 you@100.64.0.10
# Open http://127.0.0.1:18080 in the local browser.
```

The loopback origin is always accepted alongside the configured one, with the
same pairing, device authentication and Origin checks.

**Install as an app.** Settings → Host → Install. Installing from another
device needs trusted HTTPS. On iOS, use Safari → Share → Add to Home Screen,
then pair the installed app if its storage is separate. Settings → Host →
**Apply update** offers a new version; it never reloads by itself over a
draft or a pending decision.

## Notifications

Turn them on per device under Settings → Host → Notifications. A
notification is sent when a turn completes, a decision waits on you or a
conversation fails. It says only that µAgent needs your attention, never a
prompt or an answer, and opens the conversation at its decision.

| Mode | When | Needs |
| --- | --- | --- |
| Background push | The app may be closed | `UAGENT_WEB_PUSH_CONTACT` set to a `mailto:` or `https://` contact, a build with Web Push, a browser with push support |
| Connected view | Only while this page stays connected | Nothing on the host |

Both need a secure context (HTTPS or loopback) and the browser's permission.
On iOS, enable them from the installed Home Screen app. Without background
push, the pane says why. **Send test notification** checks delivery, which is
best effort; the conversation remains authoritative.

Web Push is built into release archives and the Docker image. Source builds
opt in with `-DUAGENT_WEB_PUSH=ON` and need OpenSSL 3.

## The agent's browser

The web host can run a Google Chrome that the agent drives and that you can
watch, take over and sign in to from any paired device, in the Docker image
or on a Linux host. [The agent's browser](BROWSER.md) covers setup, profiles
and saved logins, the viewer and isolation.

## Execution and persistence

A conversation has one runtime, whichever interface started it. The terminal
and the web can both send messages, add guidance, answer approvals, change
settings and inspect activity. See [Architecture](ARCHITECTURE.md) and
[Persistence](PERSISTENCE.md).

| Action | Effect |
| --- | --- |
| Close the tab, detach a terminal, restart the web host | The runtime keeps running |
| **Stop** | Interrupts the current turn |
| **Close conversation** (conversation menu) | Saves the conversation, then stops the runtime and its supervised children |
| 15 minutes with nothing to do | The runtime stops the same way by itself |

"Nothing to do" means no turn, no queued message, no running command and no
terminal attached. The next message, or a change to the conversation's
settings (model, approval mode, tools), starts a runtime from the saved
conversation with its model and approval mode. It never repeats an earlier
command. Commands started with `detach` keep running through both.

On Linux, a web host launched by systemd starts each runtime in its own user
scope through `systemd-run` (254 or newer), so a service restart does not
kill it. Keep the service's normal kill policy; Close owns session shutdown.

When the connection drops or the page reloads:

- A reload paints the app's frame before any script runs, then the page it
  showed (a conversation, Library or Scheduled) drawn from samples, and the
  conversation fills it in place. The conversation is asked for together
  with the conversation list.
- A host that cannot be reached keeps the app on screen and says so while it
  tries again; only a host that refuses this device shows the pairing form.
- The status line shows "Reconnecting…". The conversation resumes where it
  was, and controls stay disabled until the browser has caught up.
- A sent message shows as pending until the runtime accepts it. Reconnecting
  does not send it twice.
- A message the host refused or never confirmed stays at its row with the
  reason, **Retry** and **Return to composer**. A failed decision reply stays
  on the decision with **Retry**.

Cost and the context counter work as in the terminal; see
[Budgets and accounting](OPERATIONS.md#budgets-and-accounting).

## Conversation controls

### Status and decisions

A conversation with a pending decision shows "Needs your input". Decisions
waiting on you are counted once, across every folder:

- The count heads the sidebar ("2 need you") and opens every waiting
  decision, with its folder and question, to answer there or open in place.
- It also shows in the tab title ("(2) µAgent") and, where the browser
  supports it, on the installed app's badge.
- Answering there or in the conversation is the same act; the first answer
  wins.

An approval opens above the input, which stays in place, read-only, until it
is answered; the **Approval** control stays usable. The card shows what the
call would do (the command, or the change as a diff that scrolls past about
six lines), the folder and its risks, with **Deny**, **Allow for this
conversation** and **Allow once** in one row. Under **More options**,
**Always allow this exact action here** makes Allow once a rule for the
repository, listed under Settings → This project → *Allowed actions*, and
**+ guidance** answers in words instead. Numbered choices are cards.

The agent's questions are answered a page at a time. The steps above the page
show which are answered and lead back to any already seen. A tap on a single
choice moves on, and three or more questions end on a review of every answer
before Submit.

### Composer

- The model control combines provider/model and reasoning effort. Choosing
  applies to this conversation and sends nothing; **Also use for new
  conversations** makes it the default.
- Enter sends; Shift+Enter inserts a newline. On a touch keyboard Enter
  inserts a newline and the Send button sends.
- While a turn runs, Enter adds guidance to it and Esc stops it; with an
  empty draft the Send button is Stop. **Queue next** (Alt+Enter), on the
  status line once there is a draft, holds the message until the turn ends
  and then runs it as its own turn.
- After a turn stopped short (Stop, an error, a step or budget limit) the
  status line reads **Stopped** with **Continue**, which sends `continue`.
- `/` opens slash commands with suggestions and Tab completion. The list
  offers the commands the page has no control for; others still run when
  typed. `/restart` does the same as the menu; `/quit` closes nothing here;
  `/panel` opens the lead agent. A terminal command the page has no equivalent
  for (`/clear`, `/board`, `/open`) says so instead of running.
- Typed without an argument, `/config`, `/verbosity`, `/permissions`,
  `/mcp`, `/tools`, `/rename`, `/memory`, `/skills`, `/schedule`, `/context`
  and `/sessions` open their screen. With an argument they run on the host,
  as in the terminal.

### Transcript

- A turn that changed files ends with its receipt (files, lines, cost, time).
  It opens the changed files, each with **Undo**, and **Undo all**; a file
  changed since is kept and says why. Changes made by shell commands are not
  tracked.
- Tool calls that read as one step fold into one row ("Explored · 4 calls")
  that expands to the calls. **Detail** in the conversation menu sets how
  much work is shown, for every conversation and the terminal;
  [Tools](TOOLS.md#presentation) lists the levels.
- Message menus expose retained tool input and output and HTTP request and
  response captures. Credential headers are redacted; bodies can contain
  sensitive workspace content.
- Background completions, memory updates and compaction appear as expandable
  event rows. Subagent views work like the main conversation. Ordinary
  subagent follow-ups can select another model; persistent agents keep their
  runtime model. Process children show statistics from their latest saved
  checkpoint, labelled as such.
- Markdown supports code highlighting, math and fenced `mermaid` diagrams.
  Raw HTML and trusted math commands are disabled, remote images are inert
  and links use a scheme allowlist. Renderers and fonts ship locally and load
  on demand.

### Finding and managing conversations

- **Search**: Ctrl+K (⌘K on a Mac), the search button atop the sidebar or
  `/sessions` opens it, as a panel in the middle of the window (atop the
  screen on a phone). An empty field lists recent conversations. Words list
  conversations and folders by name (letters in order, prefixes first),
  then the messages that hold them (threads and panels included, tool
  output too): grouped by folder, the open conversation's first, and under
  each conversation its newest three with the words marked, then a row for
  the rest. Actions, slash commands and settings follow; `>` lists only
  those. On a wide window the rows around the selected message show beside
  the list. A message opens its conversation at that message, with find
  open on the words. Esc clears the field, then closes.
- **Find in a conversation**: Ctrl+F (⌘F), or **Find in conversation** in
  its menu, marks every occurrence in yellow, the current one stronger, and
  says which of how many it is. Enter and Shift+Enter (or the arrows) step to
  the next and previous one across the whole conversation, loading older
  messages and opening a folded tool row to reach it; Esc closes it.
- `?` outside a text field lists every shortcut. Alt+↑ and Alt+↓ step through
  the conversations in sidebar order.
- The conversation menu has **Find in conversation**, **Fork conversation**,
  **Rename**, **Statistics**, **Tools**, **Export transcript**, **Detail**,
  **Compact**, **Restart**, **Close conversation** and **Delete**. Tools
  controls the active schema and groups tools into persistent custom
  categories. Close a live runtime before deleting its history; project
  files are unaffected.

### The lead agent

[The guide](GUIDE.md#the-lead-agent) explains background agents, subagents
and the plan. In the web:

- A place's name in the sidebar, or search, opens its lead agent. The
  background agents it started are listed under the name, and a background
  agent links back to its lead agent under its title.
- The **plan** beside the chat (a button in the header on a phone) shows the
  lead agent's goal, then each step with its status. A running step is open;
  any step opens to its result, the background agents working for it and
  its subagents with their status, time and tokens, and a subagent opens in
  the inspector. Below, **Background agents** lists those no step names. A
  background agent's menu renames or removes it. What needs you is above
  the input and in the sidebar's "need you" count.

### Settings

Settings are grouped by what they affect:

| Scope | Holds |
| --- | --- |
| **All conversations** | Instructions, global MCP servers (`~/.mcp.json`), every saved setting |
| **This project** (the open conversation's folder) | Project instructions, its MCP servers (`.mcp.json`), *Allowed actions*, the settings it overrides |
| **This browser** | Appearance, Animations, Clock, Timestamps, Zoom |
| **Host** | Update, Install, Notifications, Paired devices |

- The first two list every setting under its group, with a box that filters
  them by name or variable. A switch or a choice applies as it changes; text
  and numbers are saved when you leave the field or press Enter, and Escape
  takes back what was typed.
- The arrow beside a changed setting puts it back to what applies without it.
  **Reset all to defaults** (or **Remove all overrides** for a project) does
  that for the whole scope; API keys are kept.
- A value set by the environment or the command line is shown read-only; one
  this project overrides says so. Saves reach every open browser.
- A change that needs a restart offers it: running conversations restart once
  idle and keep their history, and the web host re-executes itself for its
  own settings.
- **MCP servers** shows each server's state and tools and, when it failed,
  its reason and stderr tail. **Retry** starts a failed server again; the
  switch sets `disabled` in the file that defines the server and applies at
  once. A project file is edited only while it still matches what was
  trusted, and the trust follows that edit.

The terminal has the same through `/config`, `/restart`, `/mcp` and
`/permissions rules`. Memory, skills, schedules and instructions have their
own editors; see [Memory, skills and scheduled tasks](MANAGEMENT.md) and
[Instructions](INSTRUCTIONS.md).

### Zoom and touch

The page stays pinch-zoomable; inside the browser viewer a pinch zooms the
remote display instead. Settings → This browser → Zoom scales the whole
interface; [Accessibility](ACCESSIBILITY.md) covers zoom, contrast and
motion.

## Attachments

Attach files in the composer; what a model accepts depends on the selected
model. See [Tools](TOOLS.md) for `read_path` media input and
[Operations](OPERATIONS.md) for extraction and fallback limits.

The image viewer fits the whole image and zooms like a document viewer: the
**− / + / Fit** controls, ⌘/Ctrl+wheel or a trackpad pinch, ⌘/Ctrl with −, +
and 0, a touch pinch or a double-tap. The zoom level is relative to the
image's real size.

**Annotate** in the image viewer marks up an image to steer the agent: pen
strokes and numbered pins with a note each, drawn with mouse, trackpad,
finger or pencil and zoomed the same way (two fingers pinch, one draws).
**Attach** bakes the marks into a copy (PNG, or JPEG past the upload limit)
that replaces the draft image it was drawn on.

## Offline behavior

The app needs a connection to the host: transcripts and history are not
stored in the browser, and commands are disabled while disconnected. The
browser keeps only unsent drafts, the last conversation list (titles,
folders and workspace names) and, per tab, the page it showed. Signing out
or a revoked device clears local UI state.

## Security model

- The private `~/.uagent/web` directory holds host discovery, pairing and
  device state, push keys and subscriptions, and draft metadata. Sandboxed
  agent commands cannot read it.
- Pairing codes are single-use, expire after five minutes and are limited to
  12 attempts a minute. Pairing sets an `HttpOnly`, `SameSite=Strict` device
  cookie (also `Secure` on HTTPS).
- A paired device stays authorized for 30 days unless revoked. Revoking or
  logging out removes its access and its push subscription.
- Every request must address the configured origin or the loopback URL, and
  mutations must carry that exact Origin. API routes other than pairing
  require device authentication.
- A paired device can read the files of a conversation's folder, as the
  agent can: the [hidden stores](../SECURITY.md#sandboxing) and µAgent's own
  settings files are refused, and nothing outside the folder is served,
  through a symlink or otherwise. Images are served inline under a
  sandboxing policy; every other file, HTML included, only as a download.
- The page runs under a content security policy that allows only its own
  scripts, connections and frames. Attached HTML opens sandboxed in an opaque
  origin.

## Development

[Contributing](../CONTRIBUTING.md) has the build and the checks,
[Testing](TESTING.md#web-tests) how the web tests run and
[Architecture](ARCHITECTURE.md) how the frontend is laid out. Automated tests
do not cover a physical iOS keyboard, installation or notifications, and
browser emulation does not establish them.
