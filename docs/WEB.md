# Web interface

`uagent --web` serves µAgent to a browser: the same conversations as the
terminal, on this machine or a paired phone. This guide covers starting it,
pairing devices, remote access, notifications, the Docker browser appliance,
and reference material for operators and frontend development.

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
  host and prints a fresh code, which replaces the previous one. The running
  host keeps its port and origin; flags on the second call do not change them.
- **A refused code** (used, mistyped or older than five minutes) says it
  expired. Run `uagent --web` again.
- **Remove a device.** Settings → Host → Paired devices → **Revoke**, or
  **Log out this device**.

The sidebar lists this OS user's conversations grouped by project folder. A
folder's coordinator is its header, with the threads it started nested under
it. Browsing saved history does not start a model or run a command.

`--web` starts no conversation itself, so it refuses session flags (`-p`,
`--json`, `-c`, `--resume`, `--yolo`, `--attach`, `--model`, …). It reads the
settings saved for all conversations and the environment, never a project's.

| Flag | Setting | Default |
| --- | --- | --- |
| `--web-port PORT` | `UAGENT_WEB_PORT` | `8080` (1024–65535) |
| `--web-origin ORIGIN` | `UAGENT_WEB_ORIGIN` | empty (loopback only) |
| — | `UAGENT_WEB_BIND` | `127.0.0.1` (or `0.0.0.0`) |
| — | `UAGENT_WEB_PUSH_CONTACT` | empty (no background push) |
| — | `UAGENT_BROWSER_DATA` | empty (no shared browser) |

Save one from a terminal session with `/config user KEY=VALUE`, or set it in
the host's environment. All five apply when the web host next starts.

## Remote access and installation

The host listens on loopback. To reach it from another device, put an HTTPS
reverse proxy in front of the port and tell the host its exact public origin:

```sh
uagent --web --web-origin https://your-host.example
```

The origin is bare: scheme, host and optional port, no path. Forwarded headers
never override it, and the browser must use exactly that origin.
[Tailscale Serve](https://tailscale.com/docs/features/tailscale-serve) can
provide a private HTTPS endpoint. The proxy must forward the event stream
(`/api/events`, Server-Sent Events) and, for the
[shared browser](BROWSER.md), WebSocket upgrades on
`/api/browser/viewer`.

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
ssh -N -L 18080:127.0.0.1:18080 dev@100.64.0.9
# Open http://127.0.0.1:18080 in the local browser.
```

The loopback origin is always accepted alongside the configured one, with the
same pairing, device authentication and Origin checks.

**Install as an app.** Settings → Host → Install. Installing from another
device needs trusted HTTPS. On iOS, use Safari → Share → Add to Home Screen,
then pair the installed app if its storage is separate. An update is offered
under Settings → Host → **Apply update**; it never reloads over a draft or a
pending decision by itself.

## Notifications

Turn them on per device under Settings → Host → Notifications. A notification
is sent when a turn completes, a decision waits on you or a conversation
fails. It says only that µAgent needs your attention, never a prompt or an
answer, and opens the conversation at its decision.

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

The optional Docker image adds Google Chrome that the agent can drive and
that you can watch, take over and sign in to from any paired device.
[Browser appliance](BROWSER.md) covers setting it up, profiles and saved
logins, the viewer and how it is isolated.

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
terminal attached. The next message, or a setting changed on the conversation
(model, approval mode, tools), starts a runtime from the saved conversation
with its model and approval mode. It never repeats a previous command.
Commands started with `detach` keep running through both.

On Linux, a systemd-launched web host starts each runtime in its own user
scope through `systemd-run` (254 or newer), so a service restart does not
kill it. Keep the normal service kill policy; Close owns session shutdown.

When the connection drops or the page reloads:

- The status line shows "Reconnecting…". The conversation resumes where it
  was, and controls stay disabled until the browser has caught up.
- A sent message shows as pending until the runtime accepts it. Reconnecting
  does not send it twice.
- A message the host refused or never confirmed stays at its row with the
  reason, **Retry** and **Return to composer**. A failed decision reply stays
  on the decision with **Retry**.

Session cost follows provider-reported usage and is never invented. The
context counter is an estimate of the current request size, separate from
billing totals.

## Conversation controls

### Status and decisions

A conversation with a pending decision shows "Needs your input". Decisions
waiting on you are counted once, across every folder:

- The count heads the sidebar ("2 need you") and opens every waiting
  decision, with its folder and question, to answer or open in place.
- It also shows in the tab title ("(2) µAgent") and, where the browser
  supports it, on the installed app's badge.
- Answering there or in the conversation is the same act; the first answer
  wins.

An approval opens above the input, which stays in place, read-only, until it
is answered; Approval stays usable. The card shows what it would do (the
command, or the change as a diff, scrolling past about six lines), the folder
and its risks, with **Deny**, **Allow for session** and **Allow once** in one
row. Under **More options**, **Always allow this exact action here** makes
Allow once a rule for the repository, listed under Settings → This project →
*Allowed actions*, and **+ guidance** answers in words instead. Numbered
choices are cards.

The agent's questions are answered a page at a time. The steps above the page
show which are answered and lead back to any already seen; a tap on a single
choice moves on; three or more questions end on a review of every answer
before Submit.

### Composer

- The model control combines provider/model, variant and reasoning effort.
  Choosing does not send a message and applies to this conversation; **Also
  use for new conversations** makes it the default.
- Enter sends; Shift+Enter inserts a newline. On a touch keyboard Enter
  inserts a newline and the Send button sends.
- While a turn runs, Enter adds guidance to it and Esc stops it; with an
  empty draft the Send button is Stop. **Queue
  next** (Alt+Enter), on the status line once there is a draft, holds the
  message until the turn ends and then runs it as its own turn.
- After a turn stopped short (Stop, an error, a step or budget limit) the
  status line reads **Stopped** with **Continue**, which sends `continue`.
- `/` opens slash commands with suggestions and Tab completion. The list
  offers what the page has no control for; other commands still run when
  typed. `/restart` does the same as the menu; `/quit` closes nothing here.
- A command with its own screen opens it when typed without an argument:
  `/config`, `/verbosity`, `/permissions`, `/mcp`, `/tools`, `/rename`,
  `/memory`, `/skills`, `/schedule`, `/context` and `/sessions`. With an
  argument it runs on the host, as in the terminal.

### Transcript

- A turn that changed files ends with its receipt (files, lines, cost, time).
  It opens the changed files, each with **Undo**, and **Undo all**; a file
  changed since is kept and says why. Shell commands' changes are not
  tracked.
- Tool calls that read as one step fold into one row ("Explored · 4 calls")
  that expands to the calls. **Detail** in the conversation menu sets how
  much work is shown (`UAGENT_VERBOSITY`), for every conversation and the
  terminal: minimal shows the answer and one "Worked · N steps" row per turn,
  with failures and prompts on rows of their own.
- Message menus expose retained tool input/output and HTTP request/response
  captures. Credential headers are redacted; bodies can contain sensitive
  workspace content.
- Background completions, memory updates and compaction appear as expandable
  event rows. Subagent views work like the main conversation. Ordinary
  subagent follow-ups can select another model;
  persistent agents keep their runtime model. Process children show
  statistics from their latest saved checkpoint and label them as such.
- Markdown supports code highlighting, math and fenced `mermaid` diagrams.
  Raw HTML and trusted math commands are disabled, remote images are inert
  and links use a scheme allowlist. Renderers and fonts ship locally and load
  on demand.

### Finding and managing conversations

- Ctrl+K (⌘K on a Mac), or the search button atop the sidebar on a phone,
  opens the command palette: conversations, folders, slash commands, settings
  sections and actions, matched by letters in order (a prefix first), each
  with its shortcut.
- `?` outside a text field lists every shortcut. Alt+↑ and Alt+↓ step through
  the conversations in sidebar order.
- The conversation menu has **Fork conversation**, **Rename**,
  **Statistics**, **Tools**, **Export transcript**, **Detail**, **Compact**,
  **Restart**, **Close conversation** and **Delete**. Tools controls the active
  schema and groups tools into persistent custom categories. Stop and close a
  live runtime before deleting its history; project files are unaffected.

### The coordinator and its chat

- **New conversation → Open its coordinator** opens a folder's coordinator;
  so does the icon beside the folder in the sidebar, and the command palette.
- The **board** beside the chat (a button in the header on a phone) lists
  what needs you, the chat's members, and the threads working and done.
- Members appear with an avatar; each message carries its author's name, and
  its menu's **Show prompt** shows what that author was given. The line above
  the input says who is typing.
- Start a message with a member's name to ask that one alone.
  [The guide](GUIDE.md#the-coordinator) explains threads, members and the
  chat's rules.

### Settings

Settings are found by what they affect:

| Scope | Holds |
| --- | --- |
| **All conversations** | Instructions, global MCP servers (`~/.mcp.json`), every saved setting |
| **This project** (the open conversation's folder) | Project instructions, its MCP servers (`.mcp.json`), *Allowed actions*, the settings it overrides |
| **This browser** | Appearance, Animations, Clock, Timestamps, Zoom |
| **Host** | Update, Install, Notifications, Paired devices |

- The first two list every setting under its group, with a box that narrows
  them by name or variable. A switch or a choice applies as it changes; text
  and numbers are saved on leaving the field or Enter, and Escape takes back
  what was typed.
- The arrow beside a changed setting puts it back to what applies without it.
  **Reset all to defaults** (or **Remove all overrides** for a project) does
  that for the whole scope; API keys are kept.
- A value the environment or the command line decides is shown and cannot be
  edited; one this project overrides says so. Saves reach every open browser.
- A change that needs a restart offers it: running conversations restart once
  idle and keep their history, and the web host re-execs itself for its own
  settings.
- **MCP servers** shows each server's state, tools and, when it failed, its
  reason and stderr tail. **Retry** starts a failed server again; the switch
  sets `disabled` in the file that defines the server and applies at once. A
  project file is only edited while it still matches what was trusted, and
  trust follows that one edit.

The terminal has the same through `/config`, `/restart`, `/mcp` and
`/permissions rules`. Memory, skills, schedules and instructions have
dedicated editors; see [Memory, skills and scheduled tasks](MANAGEMENT.md) and
[Instructions](INSTRUCTIONS.md).

### Zoom and touch

The page stays pinch-zoomable; inside the browser viewer a pinch zooms the
remote display instead. Settings → This browser → Zoom scales the whole
interface from 50 to 200%. A reduced-motion preference, or Animations: Off,
disables animation. See [Accessibility](ACCESSIBILITY.md).

## Attachments

Attach files in the composer; what a model accepts depends on the selected
model. See [Tools](TOOLS.md) for `read_path` media input and
[Operations](OPERATIONS.md) for extraction and fallback limits.

The image viewer fits the whole image and zooms like a document viewer: the
**− / + / Fit** controls, ⌘/Ctrl+wheel or a trackpad pinch, ⌘/Ctrl with −, +
and 0, a touch pinch or a double-tap; the level reads against the image's real
size.

**Annotate** in the image viewer marks up an image to steer the agent: pen
strokes and numbered pins with a note each, drawn with mouse, trackpad, finger
or pencil, zoomed the same way (two fingers pinch; one draws). **Attach** bakes
the marks into a copy that joins the draft (as PNG, or JPEG past the upload
limit) and replaces the draft image it was drawn on.

## Offline behavior

A connection to the host is required: transcripts and history are not stored
in the browser, and commands are disabled while disconnected. The browser
keeps only unsent drafts and the last conversation list (titles and folders).
Signing out or a revoked device clears local UI state.

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
- The page runs under a content security policy that allows only its own
  scripts, connections and frames. Attached HTML opens sandboxed in an opaque
  origin.

## Development

[Contributing](../CONTRIBUTING.md) has the build and the checks,
[Testing](TESTING.md#web-tests) how the web tests run and
[Architecture](ARCHITECTURE.md) how the frontend is laid out. Physical iOS
keyboard, installation and notification behavior is not covered by automated
tests; browser emulation does not establish it.
