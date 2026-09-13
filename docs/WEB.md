# Web interface

Run `uagent --web`, open the printed URL, and enter the single-use pairing code.
Another invocation reuses the running server and prints a fresh code. The sidebar
lists this OS user's conversations grouped by project directory. Browsing saved
history does not start a model or execute a command.

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
responses. Offline views stop animation and disable commands.

## Conversation controls

- The composer combines provider/model, variant and reasoning effort in one
  popup. Changing a selection does not submit a message.
- `/` commands use the native registry, suggestions and Tab completion. Enter
  sends; Shift+Enter inserts a newline. Navigation and file picking belong to the
  client; execution and configuration use the runtime commands.
- Conversation menus provide rename, fork, close, delete and statistics. Stop and
  close a live runtime before deleting its history. Project files are unaffected.
- The context counter prepares the current context when idle and shows the
  captured request while running. Message menus expose retained
  tool input/output and HTTP request/response captures. Credential headers are
  redacted; bodies can contain sensitive workspace content.
- Background completions, memory updates and compaction use expandable event
  rows, with their own retained details rather than tool inspection. Subagent
  details reuse the main conversation renderer and include the full task,
  effective system prompt and child conversation. Compaction leaves the
  conversation in place without opening a dialog.
- Settings contain appearance, interface scale, text size, default permissions
  and registered configuration. Memory, skills, schedules and system prompts
  have dedicated editors; see [Management](MANAGEMENT.md) and
  [System prompts](SYSTEM_PROMPTS.md).

Markdown supports code highlighting, math and fenced `mermaid` diagrams. Copy
controls belong to code blocks. HTML and trusted math commands are disabled;
remote images are inert and links use a scheme allowlist. Renderers and fonts
ship locally and load on demand. Tables scroll within the message rather than
forcing columns below their intrinsic minimum width.
Dialogs, drawers and popovers share the visible viewport's safe-area bounds;
dialog headers stay fixed while their bodies scroll above the phone's home
indicator.

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

IndexedDB caches bounded transcript pages by authenticated host and conversation.
The service worker caches the public app shell separately. Previously loaded
history remains readable offline; older uncached pages and original attachments
require the host. Reconnect refreshes authoritative state before enabling writes.
The cache is an optional replica: storage failure cannot prevent online use, and
cached events never execute commands. Signing out removes the device's cache.

The UI uses a single viewport owner, safe-area insets and shared popovers/modals.
Input focus, orientation changes and returning from the background preserve the
selected conversation and draft. Loading skeletons share the final layout.
Reduced-motion preferences disable animation.

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
uagent --web --web-port 18080 --web-origin http://100.64.0.9:18080
tailscale serve --bg --tcp=18080 tcp://127.0.0.1:18080
```

Installation, offline shell loading and background push require trusted HTTPS.
On iOS, use Safari → Share → Add to Home Screen. Pair the installed app separately
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
modules render it. Shared controls, popovers, skeletons and spacing tokens keep
layout changes centralized. `quantities.ts` and native quantity helpers use
decimal display units; raw exports retain exact values. Native and browser tests
use mock providers. Physical iOS keyboard, install and notification behavior
still requires device validation; browser emulation does not establish it.
