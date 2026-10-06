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
[shared browser](#docker-browser-appliance), WebSocket upgrades on
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

## Docker browser appliance

The optional [Compose stack](../deploy/compose.yaml) builds one image with
the web host, Google Chrome Stable and TigerVNC. It gives the agent a real
browser that you can watch, drive and sign in to from any paired device.

```sh
mkdir -p workspaces
docker compose -f deploy/compose.yaml up --build -d
docker compose -f deploy/compose.yaml logs uagent
```

Open `http://127.0.0.1:8080` and pair with the code from the log; for another
code, run `docker compose -f deploy/compose.yaml exec uagent uagent --web`.

- Compose publishes the port on the host's loopback only; the web host binds
  `0.0.0.0` inside the container. Use Docker Engine 28 or newer:
  [older engines can expose a localhost-published port to nearby hosts](https://docs.docker.com/engine/network/port-publishing/).
- For a phone, put an HTTPS reverse proxy on the host and set
  `UAGENT_WEB_ORIGIN=https://your-host.example` in `.env.appliance` (next to
  `deploy/`) before starting Compose. Provider keys can go in the same file;
  keep it private.
- Projects go in `workspaces/`, mounted at `/workspaces`. On a Linux host,
  give UID 10001 read/write access to it.
- The `uagent-data` volume holds `/data/agent` (history, pairing, settings)
  and `/data/browser` (Chrome profiles). Back up the whole volume to keep
  every login.
- Chrome and Xvnc stay stopped until the browser is first used.

| Compose variable | Default | Purpose |
| --- | --- | --- |
| `UAGENT_WEB_PORT` | `8080` | Published loopback port and listener port |
| `UAGENT_WORKSPACES` | `../workspaces` | Host directory mounted at `/workspaces` |
| `UAGENT_SHM_SIZE` | `256m` | Chrome shared memory |
| `UAGENT_TMPFS_SIZE` | `512m` | `/tmp` tmpfs size |

Run `deploy/footprint.sh` before opening Chrome, during a viewer session and
after **Stop browser** to compare image size, packages, processes and
container memory/CPU. Raise `UAGENT_SHM_SIZE` or `UAGENT_TMPFS_SIZE` if the
measured workload needs more space.

### Agent and human control

The agent's `browser` tool and the viewer drive the same visible tab. Each
agent action waits for the page to settle and returns a fresh screenshot; a
tab the action opens becomes the active tab. A page that looks like a bot
check or rate limit is flagged as a suspected block, so the agent can hand
off or try another source.

| You want to | Do this |
| --- | --- |
| Watch the agent | The globe button in the conversation header opens the live screen, read-only |
| Drive yourself | **Take over** pauses the agent and enables input; **Done** hands back |
| Answer the agent's request | **Open browser** on its card opens the viewer already in your control; **Done** returns you to the chat |
| Free Chrome's memory | **Stop browser** in the status menu, shown while Chrome runs unused |

- Watching and driving share one connection, so the screen does not reload on
  a hand-over. The host serves the screen to one viewer at a time.
- While you drive, agent actions fail at once and tell the agent to call
  `request_human`.
- The agent calls `request_human` for a login Chrome has not saved, MFA, a
  captcha or bot check, or a payment confirmation. The conversation shows its
  reason. Only the device that took control can finish it, and closing the
  viewer keeps the agent waiting. A request whose conversation no longer runs
  is dropped.
- Without a pending request or profile sign-in, closing the viewer hands
  control back like **Done**.
- One conversation uses the browser at a time. It is free again when that
  conversation's turn ends, or after three minutes without a browser action.
  When a turn ends, only the tab the agent was on stays open.
- Chrome stops after 15 minutes without browser work, unless you control it
  or the agent waits for you, and starts again on the next action. Watching
  alone does not keep it running.

### Profiles and saved logins

A profile is one persistent Chrome login state, shared by the agent and the
viewer. In the status menu, **Chrome profile** selects one and **New profile**
creates another. Switching needs control (or a stopped Chrome) and restarts
Chrome in the chosen profile. No cookies are copied between profiles.

To sign in to sites: take over, then choose **Sign in to profile**. This
reopens the profile in ordinary Chrome without a debugging connection. Sign in
and complete MFA, then choose **Done** to reopen the same profile for the
agent. Chrome restores saved tabs; finish unsaved page edits first. No browser
security checks are disabled, and Google may still reject
automation-controlled browsers; see its
[supported browser guidance](https://support.google.com/accounts/answer/7675428).

Passwords follow Chrome's own model: sign the profile into your Google
account and manage logins at
[passwords.google.com](https://passwords.google.com). The agent uses a saved
login with `fill_saved`, which takes the one Chrome offers for the focused
field and says so when Chrome filled nothing. With several accounts it types
the start of the wanted one's name first. It never reads or types a password.

On disk, `/data/browser/profile` is the **Default** profile, named profiles
live under `/data/browser/profiles`, and `/data/browser/profiles.json`
records names and the current selection.

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
| **Zoom out** / **Zoom in**, **Scroll up** / **Scroll down** | The same without a gesture; scrolling needs control |
| **Keyboard** (touch) | Opens the on-screen keyboard and types live; the bar becomes Esc, Tab, arrows and a one-shot **Ctrl** |
| **Copy** / **Paste** | Copy Chrome's selection to this device / send this device's clipboard to Chrome |
| Ctrl/⌘+C, X, V (desktop) | Sync clipboards inside the viewer; ⌘ maps to Ctrl for the Linux Chrome |

The clipboard changes only on these actions and travels over the private VNC
connection. A browser that denies clipboard access shows a small card over the
screen to paste into or copy from.

### Isolation

Use an isolated Docker host. The image runs the web host, agent workers and
browser service under one nonroot UID and is designed for one human with
several paired devices.

- The service exposes neither a CDP nor a VNC TCP port. The agent reaches
  Chrome over inherited pipes, the viewer over the authenticated web host.
- Chrome's sandbox needs a narrow [seccomp profile](../deploy/NOTICE.md) that
  permits user namespace creation. Compose applies it without privileged mode
  and without disabling the sandbox. If the host disables unprivileged user
  namespaces, takeover fails and the browser stays stopped.
- With the command sandbox on, agent commands cannot read the browser data
  directory (saved logins, cookies) or connect to its sockets, and the file
  tools refuse it. `run(sandbox=false)` and a disabled sandbox lift that, as
  they lift the sandbox; yolo does not. See
  [SECURITY.md](../SECURITY.md#sandboxing).
- Screenshots draw every password field as dots, including one a site's
  reveal toggle switched to text; autofill still works. Fields in cross-site
  iframes and shadow roots are not masked.
- `back` refuses history entries that are not HTTP(S), like `open`.
- The [managed policy](../deploy/chrome-policy.json) blocks Chrome's local
  password and autofill pages, `view-source:` and DevTools.
- On Linux before 7.1 (Landlock ABI 9) the sockets stay reachable: the agent
  can drive the browser, which it may do anyway, but with the policy
  installed it reaches neither DevTools nor the password pages.

### Without Docker

The browser also runs under a native Linux web host that has
`google-chrome-stable`, `Xtigervnc` and `xauth` on its `PATH`. Set
`UAGENT_BROWSER_DATA` to a private directory outside the sandbox's writable
roots, such as `~/.uagent/browser`, and install the policy once:

```sh
sudo install -D -m 0644 deploy/chrome-policy.json \
  /etc/opt/chrome/policies/managed/uagent.json
```

Any top-level session may use the browser while the web host runs it. A
terminal or headless session needs `UAGENT_BROWSER_DATA` in its settings, as
the web host has.

## Execution and persistence

A conversation has one runtime, whichever interface started it. The terminal
and the web can both send messages, add guidance, answer approvals, change
settings and inspect activity. See [Architecture](ARCHITECTURE.md) and
[Persistence](PERSISTENCE.md).

| Action | Effect |
| --- | --- |
| Close the tab, detach a terminal, restart the web host | The runtime keeps running |
| **Stop** | Interrupts the current turn |
| **Close session** (conversation menu) | Saves the conversation, then stops the runtime and its supervised children |
| 15 minutes with nothing to do | The runtime stops the same way by itself |

"Nothing to do" means no turn, no queued message, no running command and no
terminal attached. The next message, or a setting changed on the conversation
(model, permission mode, tools), starts a runtime from the saved snapshot
with the conversation's model and permission mode. It never repeats a
previous command. Commands started with `detach` keep running through both.

On Linux, a systemd-launched web host starts each runtime in its own user
scope through `systemd-run` (254 or newer), so a service restart does not
kill it. Keep the normal service kill policy; Close owns session shutdown.

Delivery is safe to retry:

- Commands carry stable request IDs and a runtime generation. A repeated
  delivery returns the original receipt, conflicting reuse is rejected, and a
  changed generation requires a fresh snapshot.
- A reload or reconnect mid-stream resumes the same rows: the browser applies
  the host's changes to the snapshot it loaded, and the saved conversation
  remains the durable authority. Controls stay disabled until the browser
  has caught up.
- A sent message shows a pending row until the runtime accepts it. Reconnect
  checks receipts and history without resubmitting.
- A message the host refused or never confirmed stays at its row with the
  reason, **Retry** and **Return to composer**. A failed decision reply stays
  on the decision with **Retry**. Neither goes to the page's error banner.
- While the event stream reconnects, the status line shows a
  "Reconnecting…" pill.

Session cost follows provider-reported usage and is never invented. The
context counter is an estimate of the current request size, separate from
billing totals.

## Conversation controls

### Status and decisions

One status indicator is used in the sidebar and composer: hollow without a
live runtime, filled when connected, breathing while work runs. A pending
decision shows a steady indicator and "Needs your input"; a separate dot
marks unread responses. A sidebar row that waits on you or failed shows an
icon in place of the indicator, so no state rests on colour alone.

Decisions waiting on you are counted once, across every folder:

- The count heads the sidebar ("2 need you") and opens every waiting
  decision, with its folder and question, to answer or open in place.
- It also shows in the tab title ("(2) µAgent") and, where the browser
  supports it, on the installed app's badge.
- Answering there or in the conversation is the same act; the first answer
  wins. A notification opens `#session=<id>&decision=<id>`, which opens the
  conversation and focuses its decision.

An approval opens above the input, which stays in place, read-only, until it
is answered; Permissions stays usable. The card shows what it would do (the
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

- Its action row has fixed places in every turn state: Attach, the model,
  Permissions, and one primary button. That button is Send, or Stop while a
  turn runs and the draft is empty. The permission control turns red in YOLO
  mode.
- The model control combines provider/model, variant and reasoning effort.
  Choosing does not send a message and applies to this conversation; **Also
  use for new conversations** makes it the default.
- Enter sends; Shift+Enter inserts a newline. On a touch keyboard Enter
  inserts a newline and the Send button sends.
- While a turn runs, Enter adds guidance to it and Esc stops it. **Queue
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
  event rows. Subagent views reuse the main conversation renderer, statistics
  and input. Ordinary subagent follow-ups can select another model;
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
  **Restart**, **Close session** and **Delete**. Tools controls the active
  schema and groups tools into persistent custom categories. Stop and close a
  live runtime before deleting its history; project files are unaffected.

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
`/permissions rules`. Memory, skills, schedules and system prompts have
dedicated editors; see [Memory, skills and scheduled tasks](MANAGEMENT.md) and
[System prompts](SYSTEM_PROMPTS.md).

### Zoom and touch

The page stays pinch-zoomable; inside the browser viewer a pinch zooms the
remote display instead. Settings → This browser → Zoom scales the whole
interface from 50 to 200%. On touch devices, fields keep a layout font of at
least 16px to avoid focus zoom. A reduced-motion preference, or Animations:
Off, disables animation. See [Accessibility](ACCESSIBILITY.md).

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

## Offline behavior

There is no offline conversation storage. Transcripts and history need a live
host connection; while disconnected, commands are disabled until reconnect
refreshes authoritative state. The browser keeps only unsent drafts and the
last conversation list (titles and folders, without scheduled runs), so a
reload paints the sidebar, title and composer at once, inert until the host
answers. The service worker precaches the shell and core conversation assets;
optional renderers enter a bounded runtime cache after first use. Signing out
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
- The page runs under a content security policy that allows only its own
  scripts, connections and frames. Attached HTML opens sandboxed in an opaque
  origin.

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
