# Web interface

This guide covers running the µAgent web interface, its security model, the
Docker browser appliance and frontend development.

## Quick start

Run `uagent --web` and open the printed `<origin>/#pair=<code>` link, which
pairs the browser on opening and then drops the code from the address; or open
the URL and enter the single-use pairing code. A refused code (used, mistyped
or older than five minutes) says it expired: run `uagent --web` again.
Another invocation reuses the running server and prints a fresh code. The
sidebar lists this OS user's conversations grouped by project directory; a
folder's coordinator is its header, with the threads it started nested under
it.
Browsing saved history does not start a model or execute a command.

| Flag | Setting | Default |
| --- | --- | --- |
| `--web-port PORT` | `UAGENT_WEB_PORT` | `8080` |
| `--web-origin ORIGIN` | `UAGENT_WEB_ORIGIN` | empty (loopback only) |
| — | `UAGENT_WEB_BIND` | `127.0.0.1` |

## Remote access and installation

For remote access, set the exact trusted HTTPS origin and put a reverse proxy
in front of the loopback port:

```sh
uagent --web --web-origin https://your-host.example
```

Origin settings belong in user configuration; forwarded headers do not
override them. The browser must reach that exact origin.
[Tailscale Serve](https://tailscale.com/docs/features/tailscale-serve) can
provide a private HTTPS endpoint.

For browser testing without installation, an HTTP origin with a literal IPv4
address in the tailnet range `100.64.0.0/10` is also accepted:

```sh
uagent --web --web-port 18080 --web-origin http://100.64.0.10:18080
tailscale serve --bg --tcp=18080 tcp://127.0.0.1:18080
```

Tailnet HTTP is not a browser secure context. To test service-worker
installation without trusted HTTPS, forward the port and open the loopback URL:

```sh
ssh -N -L 18080:127.0.0.1:18080 dev@100.64.0.9
# Open http://127.0.0.1:18080 in the local browser.
```

The loopback origin is accepted alongside the configured remote origin, with
the same pairing, device authentication and exact Origin checks. Installing
directly from another device requires trusted HTTPS. On iOS, use Safari →
Share → Add to Home Screen, and pair the installed app separately if its
storage is separate. Updates are offered explicitly and do not force a reload
over drafts or a pending decision.

## Security model

- The private `~/.uagent/web` directory holds host discovery, pairing/device
  state and draft metadata.
- Pairing codes are single-use and expire after five minutes. A paired device
  stays authorized for 30 days unless revoked. Revoking a device removes its
  access and push subscription.
- Every request must address the configured origin or the loopback URL, and
  mutations must carry that exact Origin. API routes other than pairing
  require device authentication.
- Web Push is built into release archives and the Docker image (source
  builds opt in with `-DUAGENT_WEB_PUSH=ON` and OpenSSL 3). It needs a
  `UAGENT_WEB_PUSH_CONTACT` (`mailto:` or HTTPS contact) and browser
  permission. Notifications contain generic activity information, not prompts
  or answers. Delivery is best effort; the conversation remains authoritative.

## Docker browser appliance

The optional [Compose stack](../deploy/compose.yaml) builds the frontend, the
native web host, Google Chrome Stable and TigerVNC into one image. The browser
service starts with the web host but leaves Chrome and Xvnc stopped until first
use. Chrome profiles live under `/data/browser`; agent history, pairing state
and settings live under `/data/agent`. Workspaces mount separately at
`/workspaces` so the data volume does not absorb project files. On a Linux
host, give UID 10001 read/write access to the workspace directory.

```sh
mkdir -p workspaces
docker compose -f deploy/compose.yaml up --build -d
docker compose -f deploy/compose.yaml logs uagent
```

Open `http://127.0.0.1:8080` and pair with the printed code. Compose publishes
only the host loopback address; the web host binds `0.0.0.0` inside the
container. Use Docker Engine 28 or newer: [older engines can expose a
localhost-published port to nearby hosts](https://docs.docker.com/engine/network/port-publishing/).

For a phone, put an HTTPS reverse proxy on the host and set
`UAGENT_WEB_ORIGIN=https://your-host.example` in `.env.appliance` before
starting Compose. The proxy must forward WebSocket upgrades on
`/api/browser/viewer` as well as ordinary requests and SSE. Provider keys can
also go in `.env.appliance`; keep that file private.

| Compose variable | Default | Purpose |
| --- | --- | --- |
| `UAGENT_WEB_PORT` | `8080` | Published loopback port and listener port |
| `UAGENT_WORKSPACES` | `../workspaces` | Host directory mounted at `/workspaces` |
| `UAGENT_SHM_SIZE` | `256m` | Chrome shared memory |
| `UAGENT_TMPFS_SIZE` | `512m` | `/tmp` tmpfs size |

### Agent and human control

The web-only `browser` tool drives the same visible tab as the viewer using
native CDP over Chrome's inherited pipes. Each action waits for the page to
settle and returns a fresh screenshot; a tab the action opens becomes the
active tab. Pages that look like a bot check or rate limit are flagged as a
suspected block, so the agent can hand off or try another source.

Opening the browser while the agent works shows the live screen, read-only.
**Take over** pauses the agent and enables local input; **Done** hands control
back. Watching and driving share one connection, so the screen never reloads
on a hand-over. While you drive, agent actions fail at once and tell the
agent to call `request_human`. When the agent calls `request_human` (login,
MFA, a captcha or bot check, a payment confirmation), the conversation shows
its reason and **Open browser**, which opens the browser already in your
control; **Done** returns you to the chat. Only the paired device that took
control can finish it. Closing the viewer or losing its connection keeps the
agent paused. Chrome stops by itself after 15 minutes without browser work,
unless you control it or the agent is waiting for you, and starts again on the
next action; watching alone does not keep it running. A request whose
conversation no longer runs is dropped. When a turn ends, only the tab the
agent was on stays open. **Stop browser** in the status menu releases it at
once.

**Chrome profile** selects which login the agent and viewer share; **New
profile** creates another persistent login. Switching requires control and
restarts Chrome in the chosen profile. `/data/browser/profile` is the
**Default** profile, named profiles live under `/data/browser/profiles`, and
`/data/browser/profiles.json` records names and the current selection. Back up
the whole `/data` volume to preserve every login.

For account setup, take over and choose **Sign in to profile** in the status
menu. This reopens the selected profile in ordinary Chrome without a debugging
connection. Sign in and complete MFA, then choose **Done** to reopen the same profile for the agent.
Chrome restores saved tabs; finish unsaved page edits before switching. No
cookies are copied between profiles and no browser security checks are
disabled. Google may still reject automation-controlled browsers; see its
[supported browser guidance](https://support.google.com/accounts/answer/7675428).

### Viewer controls

The screen takes the whole browser sheet; one bar below holds the status menu
(page, profiles, stop), the tools and **Take over**/**Done**. On a phone the
sheet is full screen in both orientations.

| Control | Behavior |
| --- | --- |
| Tap (touch) | Clicks where the finger lands |
| Drag (touch) | Scrolls the page under the finger |
| Hold, then drag (touch) | Holds the left button: select text, move sliders, press and hold |
| Two-finger tap (touch) | Right-click |
| Pinch (touch) | Zooms and pans this device's view; nothing reaches Chrome |
| **Keyboard** (touch) | Opens the on-screen keyboard and types live; the bar becomes Esc, Tab, arrows and a one-shot **Ctrl** |
| **Copy** / **Paste** | Copy Chrome's selection to this device / send this device's clipboard to Chrome |
| Ctrl/⌘+C, X, V (desktop) | Sync clipboards inside the viewer; ⌘ maps to Ctrl for the Linux Chrome |

The clipboard changes only on these actions and travels over the private VNC
connection. A browser that denies clipboard access shows a small card over the
screen to paste into or copy from.

### Isolation

The service exposes neither a CDP nor a VNC TCP port. Chrome's sandbox needs a
narrow [seccomp profile](../deploy/NOTICE.md) that permits user namespace
creation; Compose applies it without privileged mode or disabling the sandbox.
If the host disables unprivileged user namespaces, takeover fails and the
browser stays stopped.

The image runs the web host, agent workers and browser service under one
nonroot UID and is designed for one human with multiple paired devices.
Pairing protects the web routes; the agent's commands are kept out of the
browser profile separately:

- With the sandbox on, agent commands cannot read the browser data directory
  (saved logins, cookies) or connect to its sockets, and the file tools refuse
  it; yolo, `run(sandbox=false)` and a disabled sandbox lift that, as they
  lift the sandbox. See [SECURITY.md](../SECURITY.md#sandboxing). Keep it
  outside the sandbox's writable roots, e.g. `~/.uagent/browser`.
- Screenshots draw every password field as dots, including one a site's
  reveal toggle switched to text; autofill still works. Fields in cross-site
  iframes and shadow roots are not masked.
- `back` refuses history entries that are not HTTP(S), like `open`.

Passwords follow Chrome's own model: sign the profile into your Google account
and manage logins at [passwords.google.com](https://passwords.google.com). The
managed policy blocks Chrome's local password and autofill pages,
`view-source:` and DevTools. The Docker image contains it; on a native install,
copy it once:

```sh
sudo install -D -m 0644 deploy/chrome-policy.json \
  /etc/opt/chrome/policies/managed/uagent.json
```

On Linux before 7.1 (Landlock ABI 9) the sockets stay reachable: the agent can
drive the browser, which it may do anyway, but with the policy installed it
reaches neither DevTools nor the password pages.

Use an isolated Docker host.

Run `deploy/footprint.sh` before opening Chrome, during a viewer session and
after **Stop browser** to compare image size, packages, processes and container
memory/CPU on the deployment host. Raise `UAGENT_SHM_SIZE` or
`UAGENT_TMPFS_SIZE` if the measured workload needs more space.

## Execution and persistence

A conversation has one runtime, regardless of which interface started it. The
terminal connects over a private Unix socket; the web server adapts the same
protocol to authenticated HTTP commands and SSE events. Both can submit
messages, provide guidance, answer approvals, change settings and inspect
activity. See [Architecture](ARCHITECTURE.md) and
[Persistence](PERSISTENCE.md).

Closing a tab, detaching a terminal or restarting the web server leaves the
runtime running. **Stop** interrupts the current turn. **Close** saves the
conversation, then stops the runtime and its supervised children. A runtime
with nothing to do for 15 minutes (no turn, no queued message, no running
command, no terminal attached) stops the same way by itself. Either way the
next message starts a runtime from the saved snapshot, with the conversation's
model and permission mode; it never repeats a previous command. Commands
started with `detach` keep running through both.

On Linux, a systemd-launched web host starts each runtime in its own user scope
through `systemd-run` (254 or newer), so a service restart does not kill it.
Keep the normal service kill policy; Close owns session shutdown.

Commands carry stable request IDs and a runtime generation. Repeated delivery
returns the original receipt; conflicting reuse is rejected; a changed
generation requires a fresh snapshot. A bounded event suffix lets clients join
during a turn, and the saved conversation remains the durable replay authority.
The host folds the runtime's events into its view of the conversation and sends
browsers what changed as `block` frames: a whole row, fields to set, or text to
append to a streaming row. A browser applies them to the snapshot it loaded, so
a reload mid-stream resumes the same rows instead of re-deriving them.
Browser mutations stay disabled until the server's `ready` watermark and the
selected snapshot are applied.

Submitted messages show a pending row until the runtime assigns a message ID.
Reconnect checks receipts and history without resubmitting. A message the host
refused or never confirmed stays at its row with the reason, **Retry** and
**Return to composer**; a failed decision reply stays on the decision with
**Retry**. Neither goes to the page's error banner. While the event stream
reconnects, the status line shows a “Reconnecting…” pill. Session cost follows provider-reported usage and is never
invented. The context counter is an estimate of the current request size,
separate from billing totals.

One status indicator is used in the sidebar and composer: hollow without a live
runtime, filled when connected, breathing while work runs. A pending decision
shows a steady indicator and “Needs your input”. A separate dot marks unread
responses. In the sidebar a row that waits on you or failed shows an icon in
place of the indicator, and a working row keeps the breathing indicator, so
each row has one mark and no state rests on colour alone.

The agent's questions are answered a page at a time: the steps above the page
show which are answered and lead back to any already seen, a tap on a single
choice moves on, and three or more questions end on a review of every answer
before Submit.

Decisions waiting on you are counted once, across every folder. The count
heads the sidebar (“2 need you”), which opens every waiting decision with its
folder and question to answer or open in place, and shows in the tab title
(“(2) µAgent”) and, where the browser supports it, the installed app's badge.
Answering here or in the session is the same act; the first answer wins. A
notification opens `#session=<id>&decision=<id>`, which opens the session and
focuses its decision.

## Conversation controls

- The composer combines provider/model, variant and reasoning effort in one
  popup. Changing a selection does not submit a message.
- `/` commands use the native registry, suggestions and Tab completion. Enter
  sends; Shift+Enter inserts a newline. While a turn runs, Enter adds guidance
  to it and Esc stops it; **Queue next** (Alt+Enter) holds the message until
  the turn ends and then runs it as its own turn. The permission control turns
  red in YOLO mode.
- The composer is one shape in every turn state. Its action row has fixed
  places: Attach, the model (the only one that stretches), Permissions, and
  one primary button, which is Send, or Stop while a turn runs and the draft
  is empty. The status line above it is one line: the phase on the left, and
  on the right **Queue next** (while a turn runs and there is a draft) or
  **Continue** (after a stop).
- An approval opens above the input, which stays in place, read-only, until
  it is answered; Permissions stays usable. The card shows what it would do
  (the command, or the change as a diff, scrolling past about six lines), the
  folder and its risks, with **Deny**, **Allow for session** and **Allow
  once** in one row. Under **More options**, **Always allow this exact action
  here** makes Allow once a rule for the repository, listed under *Allowed
  actions* in Settings → Permissions, and **+ guidance** answers in words
  instead. Numbered choices are cards.
- After a turn stopped short (Stop, an error, a step or budget limit) the
  status line reads **Stopped** with **Continue**, which sends `continue`.
- A turn that changed files ends with its receipt (files, lines, cost, time).
  It opens the changed files, each with **Undo**, and **Undo all**; a file
  changed since is kept and says why. Shell commands' changes are not
  tracked. Three or more tool calls in a row fold into one row (“Ran 4
  commands · edited 2 files”) that expands to the calls.
- Ctrl+K (⌘K on a Mac), or the search button atop the sidebar on a phone,
  opens the command palette: conversations, folders, slash commands, settings
  sections and actions, matched by letters in order (a prefix first), each with
  its shortcut. `?` outside a text field lists every shortcut; Alt+↑ and Alt+↓
  step through the conversations in sidebar order.
- Conversation menus provide tools, rename, fork, statistics, export of the
  transcript, compact, restart, close and delete. Typed `/restart` does the
  same as the menu; `/quit` detaches a terminal and closes nothing here.
  The Tools view controls the active schema and groups tools into persistent
  custom categories. Stop and close a live runtime before deleting its history;
  project files are unaffected.
- Message menus expose retained tool input/output and HTTP request/response
  captures. Credential headers are redacted; bodies can contain sensitive
  workspace content.
- Background completions, memory updates and compaction appear as expandable
  event rows. Subagent views reuse the main conversation renderer, statistics
  and input. Ordinary subagent follow-ups can select another model; persistent
  agents keep their runtime model. Process children show statistics from their
  latest saved checkpoint and label them as such.
- Settings group their sections as General; Agent (Instructions, Tools, MCP
  servers, Permissions & allowed actions); Models; Host (Devices); and
  Advanced. General holds this device's display settings. Every other
  setting is a row with its name and the value that applies: a switch flips
  in place, anything else opens a sheet with what it is for, the value, why
  it applies, **Use default** and **Save**. Advanced lists what you changed
  and what the environment or command line locks; its search finds any other
  setting. The web edits your defaults; a project's override is shown and is
  edited in its `.uagent/.config` or with `/config project`. **Reset all to
  defaults** (Advanced) also resets this device's display settings; API keys
  are kept. A change that needs a restart offers it: running conversations
  restart once
  idle and keep their history, and the web host re-execs itself for its own
  settings. The terminal has the same through `/config`, `/restart`, `/mcp`
  and `/permissions rules`. **MCP servers** lists the open conversation's
  servers under *Global* (`~/.mcp.json`) or *This project* (`.mcp.json`),
  with each one's state, tools and, when it failed, its reason and stderr
  tail. **Retry** starts a failed server again; the switch sets `disabled` in
  the file that defines the server and applies at once. A project file is
  only edited while it still matches what was trusted, and trust follows
  that one edit. Memory, skills, schedules and system prompts have
  dedicated editors; see [Library and scheduled tasks](MANAGEMENT.md) and
  [System prompts](SYSTEM_PROMPTS.md).

Markdown supports code highlighting, math and fenced `mermaid` diagrams. Raw
HTML and trusted math commands are disabled, remote images are inert and links
use a scheme allowlist. Renderers and fonts ship locally and load on demand.

The page stays pinch-zoomable. While the browser viewer is open the viewport
adds `maximum-scale=1, user-scalable=no`, so a pinch zooms the remote display
rather than the page. On touch devices, fields keep a layout font of at
least 16px to avoid focus zoom. The installed PWA uses layout bounds at rest and
visual-viewport bounds while the keyboard is open; rotation, visibility changes
and page restoration refresh them. Reduced-motion preferences disable
animation.

## Attachments

The native attachment pipeline inspects original files and projects them for
the selected model's declared capabilities. Images use native image input when
supported, or the configured vision fallback. Supported document routes receive
native document input; other routes use bounded extraction or an explicit
unsupported-format error. Audio and video are sent natively only on Chat
Completions routes; other dialects receive the file path.

Web uploads are private session assets; local `/attach PATH` uses the same
inspection. Uploads are claimed before the prompt is accepted, so cleanup
cannot remove a referenced file. See [Tools](TOOLS.md) for `read_path` media
input and [Operations](OPERATIONS.md) for extraction and fallback limits.

The image viewer fits the whole image and zooms like a document viewer: the
**− / + / Fit** controls, ⌘/Ctrl+wheel or a trackpad pinch, ⌘/Ctrl with −, +
and 0, a touch pinch or a double-tap; the level reads against the image's real
size.

**Annotate** in the image viewer marks up an image to steer the agent: pen
strokes and numbered pins with a note each, drawn with mouse, trackpad, finger
or pencil, zoomed the same way (two fingers pinch; one draws). **Attach** bakes
the marks into a copy that joins the draft (as PNG, or JPEG past the upload
limit) and replaces the draft image it was drawn on.

A slash command with its own screen opens it when typed without an argument:
`/config`, `/permissions`, `/mcp`, `/tools`, `/rename`, `/memory`, `/skills`,
`/schedule`, `/context` and `/sessions`. With an argument it runs on the host,
as in the terminal.

## Offline behavior

There is no offline conversation storage. Transcripts and history need a live
host connection; while disconnected, commands are disabled until reconnect
refreshes authoritative state. The browser keeps only unsent drafts and the
last conversation list (titles and folders, without scheduled runs), so a
reload paints the sidebar, title and composer at once, inert until the host
answers. The service worker precaches the shell and core conversation assets;
optional renderers enter a bounded runtime cache after first use. Signing out
or a revoked device clears local UI state.

## Bounds

| Resource | Bound |
| --- | --- |
| IPC frame / command | 1 MiB / 512 KiB |
| Runtime output queue / host replay | 4 MiB each |
| Recent command receipts | 256 per runtime |
| Session file / session header | 64 MiB / 16 KiB |
| Catalogue / active history page | 4,096 sessions / 64 blocks |
| Upload / files per prompt | 8 MiB / 8 |
| Session / global source assets | 64 MiB / 512 MiB |
| Paired devices / device lifetime | 16 / 30 days |
| Event streams (SSE) | 4 concurrent per host |
| Pairing code lifetime | 5 minutes |

## Development

Native builds embed the built `web/dist` (not committed; CI builds it once for
every native job, and releases ship it). The frontend uses strict TypeScript
and Preact.

```sh
npm ci --prefix web
npm run test --prefix web
npm run build --prefix web
cmake --preset release
cmake --build --preset release --parallel 4
npm run test:browser --prefix web
```

| Script | Purpose |
| --- | --- |
| `test` | Node unit tests (`web/tests/*.test.js`) |
| `test:browser` | Playwright tests against mock providers |
| `build` | Type-check, build and promote `web/dist` |
| `typecheck` | App and service-worker type checks |
| `format` / `format:check` | Prettier |
| `size` | Report raw and gzip bundle sizes (advisory, no ceiling) |
| `notices` / `icons` | Regenerate third-party notices / icons |

### Component ownership

| Concern | Owner |
| --- | --- |
| Connection and replay | `web/src/state/use-host.ts` |
| Event projection | `web/src/state/store.ts` |
| Command and management transport | `web/src/state/api.ts` |
| Cache and rendering budgets | `web/src/shared/limits.ts` |
| Decimal display units | `web/src/shared/quantities.ts` |
| Viewer gesture tuning | `web/src/features/browser/gestures.ts` |

Feature modules render the store. Shared controls, popovers, spinners and
spacing tokens keep layout changes centralized; editable fields must use the
shared `Input`, `Textarea` and `Select` controls (`web/tests/form-controls.test.js`
rejects native fields). The `Modal` shell owns dialog size, so loading and
loaded states share geometry. A loading state is the loaded view itself,
rendered from sample data inside `<Placeholder>` (`web/src/shared/placeholder.tsx`):
primitives draw their data text as bars and the subtree is inert, so a screen
and its loading state cannot drift apart. Text of unknown length uses
`Skeleton`, and unknown content (a document, a live screen) uses one spinner.
The shell, sidebar, transcript and composer ship in the entry bundle and render
from the first frame; dialog and page chunks are warmed shortly after boot.

The UI showcase renders the real shared components, including the browser
viewer controls, without a host connection for visual review. It is a
development page, not part of the product: `npx vite` in `web/` serves it at
`/ui.html`, and the Playwright configuration starts that server for its tests.

`web/tests/performance.spec.js` streams 5,000 mock tokens through the real
EventSource handlers, checks lossless final text and samples frame gaps and
input latency:

```sh
npm run test:browser --prefix web -- performance.spec.js
```

It measures browser processing only, not provider throughput or phone
performance. Physical iOS keyboard, focus zoom, installation and notification
behavior is not covered by automated tests; browser emulation does not
establish it.
