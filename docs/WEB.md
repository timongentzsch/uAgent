# Web interface

Run `uagent --web`, open the printed URL, and enter the single-use pairing code.
Another invocation reuses the running server and prints a fresh code. The sidebar
lists this OS user's conversations grouped by project directory. Browsing saved
history does not start a model or execute a command.

## Docker browser appliance

The optional [Compose stack](../deploy/compose.yaml) builds the frontend and native web host, a
persistent Google Chrome Stable and TigerVNC in one image. The browser service
starts with the web host but leaves Chrome and Xvnc stopped until first use.
It uses private Chrome profiles under `/data/browser`; all agent history,
pairing state and settings live under `/data/agent`. Mount `/workspaces`
separately so the persistent data volume does not silently absorb project files.
On a Linux host, give UID 10001 read/write access to the mounted workspace
directory before creating conversations there.

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
starting Compose. Proxy WebSocket upgrades on `/api/browser/viewer` as well as
ordinary requests and SSE. The exact origin must match the browser address.
Provider keys can also go in `.env.appliance`; keep that file private.

The web-only `browser` tool uses the same visible tab as the viewer. Navigate,
observe, click, type and scroll use native CDP over Chrome's inherited pipes.
Opening the browser while the agent is working starts a read-only viewer, so
the page remains visible without changing browser ownership. **Take control**
is the explicit handoff that pauses agent input and enables local input.
When the agent calls `request_human`, the conversation shows **Open browser**.
Take control, complete the requested interaction, then choose **Done**. Only
that paired device can finish the matching pending interaction. Closing the
viewer or losing its connection keeps the agent paused. A second device can
take control after the first finishes; the browser profile and login survive a
web-host restart. Stop browser from the viewer when idle to release resources.
Use **Chrome profile** in the browser dialog to choose which login the agent
and viewer share. **New profile** creates a separate persistent Chrome login
and selects it. Switching while controlling the browser restarts Chrome in the
chosen profile and reconnects the viewer; complete any unsaved page work first.
When the browser is busy with the agent, take control before switching. The
existing `/data/browser/profile` remains the **Default** profile, so upgrading
does not move or clear a saved login. Named profiles live under
`/data/browser/profiles`; `/data/browser/profiles.json` records their names and
current selection. Back up the full `/data` volume to preserve every login.

For account setup choose **Take control → Sign in to profile**. This reopens
the selected persistent profile in ordinary Chrome without a debugging
connection. Sign in to your sites and complete MFA, then choose **Done** to
reopen the same profile for the agent. Chrome restores saved tabs; finish
unsaved page edits before switching modes. Profile shutdown has a bounded grace
period to flush saved state. No cookies are copied between profiles and no
browser security checks are disabled. Google may reject automation-controlled
browsers; see its [supported browser guidance](https://support.google.com/accounts/answer/7675428?co=GENIE.Platform%3DDesktop&hl=en).
Account-specific sign-in acceptance still needs a real user check.
On a phone the display is view-only: pinch to zoom and drag the zoomed view to
pan. The trackpad below it is always shown on touch devices: one finger moves
the remote pointer, a tap is a left click, a two-finger tap a right click and a
two-finger drag scrolls. Hold **Left** while moving for drag, resize or text
selection. The pointer stays visible in the browser dialog, and a zoomed
display pans to follow it. **Keyboard** opens the on-screen keyboard and types
live into Chrome; while it is open a row adds Esc, Tab, arrows and a one-shot
**Ctrl**. **Keys** offers the same keys plus **Address bar**, **Select all**
and **Send text…** for dictation or other composed input.
**Copy** copies Chrome's selection to this device and **Paste** sends this
device's clipboard into Chrome, on desktop and phone alike. On desktop,
Ctrl/⌘+C, X and V inside the viewer do the same; ⌘ acts as Ctrl for the Linux
Chrome. The clipboard changes only on those actions and travels over the
existing private VNC connection; a browser that denies clipboard access falls
back to a text field.
The service exposes neither a CDP TCP port nor a VNC TCP port.
Chrome's sandbox requires a narrow [seccomp profile](../deploy/NOTICE.md)
that permits user namespace creation. The Compose stack applies it without
privileged mode or disabling Chrome's sandbox. If the host disables
unprivileged user namespaces, takeover fails and the browser remains stopped.

The image runs the web host, agent workers and browser service under one
nonroot Unix UID. It is designed for one human using multiple paired devices.
The same UID can read the browser profile and private service socket, including
through an approved shell command. Pairing protects the web routes; it is not
an operating-system boundary between agent workers and Chrome. Use an
isolated Docker host and back up the `/data` volume when the profile matters.

The viewer and persistent profile permit interactive login, but Google login
and MFA require a manual check with the account owner on the target host. A
successful container build or page screenshot does not establish that a
particular provider accepted that sign-in. Chrome Sync is not required for
profile persistence and is not guaranteed in the container. CLI Playwright
automation remains separate from this image.

Run `deploy/footprint.sh` before opening Chrome, during a viewer session, and
after **Stop browser** to compare image size, packages, process inventory and
container memory/CPU on the same host. `UAGENT_SHM_SIZE` and
`UAGENT_TMPFS_SIZE` adjust the Compose capacities if the measured workload
needs more space.

On an ARM64 Docker Desktop host (2026-09-21), the built image measured 378 MB
in Docker and 375 MB as a gzip-compressed image archive. The stopped browser
used 15 MiB of container memory; Chrome showing a page used 828 MiB. The lazy
viewer bundle with phone controls is 205 KB raw / 61 KB gzip. These are
comparison points, not size ceilings; measure again on the deployment host.

## Execution and persistence

A conversation has one runtime, regardless of which interface started it. The
terminal connects over a private Unix socket; the web server adapts that same
protocol to authenticated HTTP commands and SSE events. Both can submit messages,
provide guidance, answer approvals, change settings and inspect activity.

Closing a tab, detaching a terminal, or restarting the web server leaves the
runtime running. **Stop** interrupts the current turn. **Close** stops the runtime
and its supervised children after saving the conversation. Detached services have
an explicit separate lifetime. Reopening a closed conversation starts a runtime
from its saved snapshot; it never repeats a previous command automatically.

On Linux, a systemd-launched client starts each runtime in a separate user scope
through `systemd-run` (254 or newer). This preserves the caller's environment and
keeps the runtime outside the web service's stop/kill group. Keep normal service
kill policy; Close owns session shutdown. See [systemd's scope semantics](https://github.com/systemd/systemd/blob/main/man/systemd-run.xml).

The runtime serializes input and publishes ordered events and checkpoints.
Commands carry stable request IDs and a runtime generation. Repeated delivery of
the same command returns its receipt; conflicting reuse is rejected. A changed
generation requires a fresh snapshot. The bounded event suffix supports clients
joining during a turn; the saved conversation remains durable replay authority.
Opening the stream establishes transport only. A `ready` watermark follows
ordered replay, and browser mutations stay disabled until that watermark and the
selected snapshot are applied. Healthy foreground transitions retain their
stream; disconnected and bfcache-restored pages share one recovery path.
See [Architecture](ARCHITECTURE.md) and [Persistence](PERSISTENCE.md).

Submitted messages have an immediate pending row, reconciled with the runtime's
message ID. Reconnect checks receipts and history without resubmitting. Explicit
rejection restores the draft. Provider-reported usage updates session costs while
a turn runs; unavailable cost is never invented. Context is a current-request
size estimate, refreshed as response content and tool arguments arrive; billing
totals are separate. Per-turn and session statistics
show the reported route, tokens, timing and tool counts.

One status indicator is used in the sidebar and composer: hollow without a live
runtime, filled when connected, breathing while work runs. A pending decision has
a steady indicator and a “Needs your input” label. A separate dot marks unread
responses. Disconnected views stop animation and disable commands.

## Conversation controls

- The composer combines provider/model, variant and reasoning effort in one
  popup. Changing a selection does not submit a message.
- `/` commands use the native registry, suggestions and Tab completion. Enter
  sends; Shift+Enter inserts a newline. Navigation and file picking belong to the
  client; execution and configuration use the runtime commands.
- Conversation menus provide tools, rename, fork, close, delete and statistics.
  The Tools view controls the active schema and lets the user group tools in
  persistent custom categories. Stop and close a live runtime before deleting
  its history. Project files are unaffected.
- The context counter prepares the current context when idle and shows the
  captured request while running. Message menus expose retained
  tool input/output and HTTP request/response captures. Credential headers are
  redacted; bodies can contain sensitive workspace content.
- Background completions, memory updates and compaction use expandable event
  rows, with their own retained details rather than tool inspection. Subagent
  details reuse the main conversation renderer, older-history control, context
  estimate, turn/session statistics, model
  picker and message input. Ordinary follow-ups can select another model;
  persistent agents retain their runtime model. Running process children label
  statistics that reflect only the latest saved checkpoint. Compaction leaves the
  conversation in place without opening a dialog.
  Context capacity is saved with each new checkpoint; older snapshots that did
  not record it show the usage estimate without inventing a capacity.
- Settings contain appearance, zoom (the entire interface, conversation
  included), default permissions and registered configuration. The UI showcase
  opens the shared controls and loading states on a static page for focused
  visual review. Memory, skills, schedules and system prompts have dedicated
  editors; see [Management](MANAGEMENT.md) and
  [System prompts](SYSTEM_PROMPTS.md).

Markdown supports code highlighting, math and fenced `mermaid` diagrams. Copy
controls belong to code blocks. HTML and trusted math commands are disabled;
remote images are inert and links use a scheme allowlist. Renderers and fonts
ship locally and load on demand. Tables scroll within the message rather than
forcing columns below their intrinsic minimum width.
Dialogs, drawers and popovers share the visible viewport's safe-area bounds;
dialog headers stay fixed while their bodies scroll above the phone's home
indicator.
Dialog focus starts at its title, and nested dialogs return focus without
outlining their container. Only interactive keyboard targets show focus rings.
All editable fields use the shared `Input`, `Textarea` and `Select` controls,
including search, settings, editors and both composers. They scale painted text
with interface density. On touch devices they retain a layout font of at least
16px and compensate a CSS transform with matching width, padding and height.
The UI showcase includes the same controls; a source check prevents screens from
adding native fields that bypass this behavior. Browser checks cover displayed
text and geometry at 50%, 75%, 100%, 150% and 200%. This follows
WebKit's use of the [layout font size](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/WebProcess/WebPage/ios/WebPageIOS.mm#L2690)
for its [focus zoom calculation](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/UIProcess/API/ios/WKWebViewIOS.mm#L1767),
while preserving user pinch zoom. Physical iPhone/PWA testing remains necessary
for the native keyboard and viewport interaction.
The standalone PWA uses CSS layout bounds at rest and visual-viewport bounds for
keyboard/pinch changes. Rotation invalidates the previous keyboard geometry;
orientation, visibility and page restoration events refresh the shared bounds.

See the [web audit](WEB_AUDIT.md) for component ownership, the performance probe
and remaining targets that need measurement or physical-device validation.

## Attachments

The native attachment pipeline inspects original files and projects them for the
selected model's declared capabilities. Images use native image input when
supported, or the configured vision fallback. Supported document routes receive
native document input; other routes use bounded extraction or an explicit
unsupported-format error. A model name does not imply input support.

Web uploads are private session assets; local `/attach PATH` uses the same
inspection and request preparation. Uploads are claimed before the prompt is
accepted so cleanup cannot remove a referenced file. Raw HTTP captures show the
actual provider representation. See [Tools](TOOLS.md) for `read_path` media input
and [Operations](OPERATIONS.md) for extraction and fallback limits.

## Offline conversations

There is no offline conversation storage: transcripts, drafts and history
require a live host connection, and disconnecting disables commands until
reconnect refreshes authoritative state. The service worker precaches the
shell and core conversation assets. Build-derived optional renderer assets
enter a bounded runtime cache after first use. Signing out clears local
UI state.

The UI uses a single viewport owner, safe-area insets and shared popovers/modals.
Input focus, orientation changes and returning from the background preserve the
selected conversation and draft. Initial conversation loading uses one stable
indeterminate spinner across authentication, module and snapshot preparation.
Skeletons are reserved for lists and forms whose final geometry is known.
Content of unknown length can still grow when it arrives. Reduced-motion
preferences disable animation.

## Phone access and installation

The native listener binds to `127.0.0.1`. For remote access, set the exact trusted
HTTPS origin and put a reverse proxy in front of the loopback port:

```sh
uagent --web --web-origin https://your-host.example
```

`--web-port` defaults to 8080. Origin settings belong in user configuration;
forwarded headers do not override them. The browser must reach that exact origin.
[Tailscale Serve](https://tailscale.com/docs/features/tailscale-serve) can provide a
private HTTPS endpoint.

For browser testing without installation, an explicit HTTP origin with a literal
IPv4 address in the tailnet range `100.64.0.0/10` is also accepted:

```sh
uagent --web --web-port 18080 --web-origin http://100.64.0.10:18080
tailscale serve --bg --tcp=18080 tcp://127.0.0.1:18080
```

Tailnet HTTP is not a browser secure context. To test service-worker installation
without trusted HTTPS, forward the same port and open the exact loopback URL:

```sh
ssh -N -L 18080:127.0.0.1:18080 dev@100.64.0.9
# Open http://127.0.0.1:18080 in the local browser.
```

The loopback origin is accepted alongside the configured remote origin, with the
same pairing, device authentication and exact Origin checks. Trusted HTTPS is
still required when installing directly from another device. On iOS, use
Safari → Share → Add to Home Screen. Pair the installed app separately
if its browser storage is separate. Updates are offered explicitly and do not
force a reload over drafts or a pending decision.

## Pairing and notifications

The private `~/.uagent/web` directory holds host discovery, pairing/device state
and draft metadata. Pairing codes are single-use and expire after five minutes.
Mutations require device authentication and the configured origin. Revoking a
device removes its access and subscription.

Optional Web Push requires `-DUAGENT_WEB_PUSH=ON`, OpenSSL 3 and explicit browser
permission. Notifications contain generic activity information, not prompts or
answers. Delivery is best effort; the conversation remains authoritative.

## Bounds and development

| Resource | Bound |
| --- | --- |
| IPC frame / command | 1 MiB / 512 KiB |
| Runtime output queue / replay | 4 MiB each |
| Recent command receipts | 256 per runtime |
| Session file / catalogue header | 64 MiB / 16 KiB |
| Catalogue / active history page | 4,096 sessions / 64 blocks |
| Upload / files per prompt | 8 MiB / 8 |
| Session / global source assets | 64 MiB / 512 MiB |
| Paired devices | 16 |

Native builds embed checked-in `web/dist`; users need no Node runtime. Frontend
work uses strict TypeScript and Preact:

```sh
npm ci --prefix web
npm run test --prefix web
npm run build --prefix web
cmake --preset release
cmake --build --preset release --parallel 4
npm run test:browser --prefix web
```

`use-host.ts` owns connection/replay, `store.ts` the event projection, and feature
modules render it. Shared controls, popovers, spinners, structural skeletons and
spacing tokens keep layout changes centralized. `/ui.html` renders those real
shared components, including the remote browser controls, without a host
connection so themes, scale, gestures and states can be reviewed in isolation.
`quantities.ts` and native quantity helpers use decimal
display units; raw exports retain exact values. Native and browser tests use mock
providers. Physical iOS keyboard, install and notification behavior still
requires device validation; browser emulation does not establish it.

### Frontend bundle baseline (`web/scripts/size.js`, CI-reported)

Measured 2026-09-23 after the browser viewer and shared UI updates. Core shell CSS is
available before lazy feature modules so loading and loaded surfaces keep their
columns and dialog bounds. `npm run size --prefix web` records raw and gzip
bytes in CI for review; it has no fixed byte ceiling. Heavy renderers
(mermaid, katex, highlight) stay in lazy chunks.

| Group | Raw | Gzip |
| --- | ---: | ---: |
| Initial JS | 70,260 | 28,076 |
| Initial CSS | 22,984 | 5,509 |
| App (excl. diagrams) | 1,172,483 | 571,293 |
| Diagrams (lazy) | 5,126,644 | 1,480,446 |
