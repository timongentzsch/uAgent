# Web interface

This guide covers running the µAgent web interface, its security model, the
Docker browser appliance and frontend development.

## Quick start

Run `uagent --web`, open the printed URL and enter the single-use pairing code.
Another invocation reuses the running server and prints a fresh code. The
sidebar lists this OS user's conversations grouped by project directory.
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
native CDP over Chrome's inherited pipes. Opening the browser while the agent
works shows a read-only viewer. **Take control** pauses agent input and enables
local input; **Done** hands control back. When the agent calls `browser` with
`request_human`, the conversation shows **Open browser**: take control,
complete the interaction, then choose **Done**. Only the paired device that
took control can finish it. Closing the viewer or losing its connection keeps
the agent paused. Use **Stop browser** when idle to release resources.

**Chrome profile** selects which login the agent and viewer share; **New
profile** creates another persistent login. Switching requires control and
restarts Chrome in the chosen profile. `/data/browser/profile` is the
**Default** profile, named profiles live under `/data/browser/profiles`, and
`/data/browser/profiles.json` records names and the current selection. Back up
the whole `/data` volume to preserve every login.

For account setup, take control and choose **Sign in to profile**. This reopens
the selected profile in ordinary Chrome without a debugging connection. Sign in
and complete MFA, then choose **Done** to reopen the same profile for the agent.
Chrome restores saved tabs; finish unsaved page edits before switching. No
cookies are copied between profiles and no browser security checks are
disabled. Google may still reject automation-controlled browsers; see its
[supported browser guidance](https://support.google.com/accounts/answer/7675428).

### Viewer controls

| Control | Behavior |
| --- | --- |
| Display (touch) | View-only: pinch to zoom, drag the zoomed view to pan |
| Trackpad (touch) | One finger moves the pointer, tap left-clicks, two-finger tap right-clicks, two-finger drag scrolls; hold **Left** to drag or select |
| **Keyboard** (touch) | Opens the on-screen keyboard and types live; adds an Esc/Tab/arrows row and a one-shot **Ctrl** |
| **Copy** / **Paste** | Copy Chrome's selection to this device / send this device's clipboard to Chrome |
| **Keys** | Esc, Tab, Enter, ⌫, arrows, **Address bar**, **Select all**, **Send text…** (for dictation or composed input) |
| Ctrl/⌘+C, X, V (desktop) | Sync clipboards inside the viewer; ⌘ maps to Ctrl for the Linux Chrome |

On touch devices the pointer is Chrome's own cursor (arrow, I-beam, hand), drawn
at the display's scale; a zoomed display pans to follow it. The clipboard
changes only on these actions and travels over the private VNC connection; a
browser that denies clipboard access falls back to a text field.

### Isolation

The service exposes neither a CDP nor a VNC TCP port. Chrome's sandbox needs a
narrow [seccomp profile](../deploy/NOTICE.md) that permits user namespace
creation; Compose applies it without privileged mode or disabling the sandbox.
If the host disables unprivileged user namespaces, takeover fails and the
browser stays stopped.

The image runs the web host, agent workers and browser service under one
nonroot UID and is designed for one human with multiple paired devices. That
UID can read the browser profile and the private service socket, including
through an approved shell command. Pairing protects the web routes; it is not
an operating-system boundary between agent workers and Chrome. Use an isolated
Docker host.

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
conversation, then stops the runtime and its supervised children. Reopening a
closed conversation starts a runtime from its saved snapshot; it never repeats
a previous command.

On Linux, a systemd-launched web host starts each runtime in its own user scope
through `systemd-run` (254 or newer), so a service restart does not kill it.
Keep the normal service kill policy; Close owns session shutdown.

Commands carry stable request IDs and a runtime generation. Repeated delivery
returns the original receipt; conflicting reuse is rejected; a changed
generation requires a fresh snapshot. A bounded event suffix lets clients join
during a turn, and the saved conversation remains the durable replay authority.
Browser mutations stay disabled until the server's `ready` watermark and the
selected snapshot are applied.

Submitted messages show a pending row until the runtime assigns a message ID.
Reconnect checks receipts and history without resubmitting; explicit rejection
restores the draft. Session cost follows provider-reported usage and is never
invented. The context counter is an estimate of the current request size,
separate from billing totals.

One status indicator is used in the sidebar and composer: hollow without a live
runtime, filled when connected, breathing while work runs. A pending decision
shows a steady indicator and “Needs your input”. A separate dot marks unread
responses.

## Conversation controls

- The composer combines provider/model, variant and reasoning effort in one
  popup. Changing a selection does not submit a message.
- `/` commands use the native registry, suggestions and Tab completion. Enter
  sends; Shift+Enter inserts a newline.
- Conversation menus provide tools, rename, fork, close, delete and statistics.
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
- Settings contain appearance, interface zoom, default permissions and
  registered configuration. Memory, skills, schedules and system prompts have
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

## Offline behavior

There is no offline conversation storage. Transcripts, drafts and history need
a live host connection; while disconnected, commands are disabled until
reconnect refreshes authoritative state. The service worker precaches the shell
and core conversation assets; optional renderers enter a bounded runtime cache
after first use. Signing out clears local UI state.

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

Native builds embed the checked-in `web/dist`; users need no Node runtime. The
frontend uses strict TypeScript and Preact.

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
loaded states share geometry; unknown content uses one spinner, and skeletons
are reserved for known shapes. `/ui.html` renders the real shared components,
including the browser viewer controls, without a host connection for visual
review.

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
