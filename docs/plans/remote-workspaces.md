# Remote workspaces over SSH

Status: implementation in progress, 2026-11-10, following the Fable 5.2 review.
The checklist below remains the acceptance contract. Local passing checks,
four native CI payloads, dev installation and GPU validation are separate gates.
See [Remote hosts](../REMOTE_HOSTS.md) for the implemented connection flow.

The first update gate is deliberately conservative: an attached runtime,
even when idle, prevents automatic host replacement until it closes or retires.
The small executable embeds the required preview helper, without the web app.

## Objective and scope

Open remote folders from the normal uAgent web interface using `user@machine`
or an SSH alias. Bootstrap automatically, remember the host and reuse its
conversations. Execution and files remain remote; desktop and phone clients
use the same interface.

- Linux/macOS on x86_64/ARM64: four native payloads from one codebase. The
  main install carries all four, ready to copy. No connect-time release
  downloads, compilation, sudo or replacement of the remote user's CLI.
- Configured SSH keys and system OpenSSH, respecting aliases, ports, identity
  selection and jump hosts. Provide actionable key/trust guidance.
- One web host and authoritative state directory per remote OS user, shared
  across folders, runtimes and clients. Different users retain separate state.
- Preserve existing chat, files, Git, terminals, approvals, browser uploads/
  downloads and static folder previews on the selected execution host.
- Work survives client/SSH disconnection. Logout/reboot persistence are
  separate, unpromised capabilities; remote browser dependencies are optional.

## Lean implementation rules

- Reuse the existing HTTP/SSE/WebSocket API through an authenticated host
  proxy. Do not invent another workspace RPC or framed stdio protocol.
- One SSH manager per saved host in the serving uAgent process. Folders and
  UI clients share it; concurrent connect/bootstrap requests join one operation.
- Each execution host owns its catalogue, pins, receipts and event log.
  Aggregate only display projections. Never republish remote events into
  Local's replay log or add an authoritative transcript store on the gateway.
- Reuse sidebar rows, pin actions, folding, selection, menus, status controls
  and shared styles. UI components do not own SSH retry timers.
- Start with the existing executable built without embedded web assets.
  Measure before splitting libraries or introducing a separate backend target.
- Remove replaced paths as consumers move. Avoid generic networking
  frameworks, cross-version adapters and unrelated refactors.

## Source baseline and proof limits

The review and follow-up source inspection establish these reuse points:

| Existing owner | Consequence |
| --- | --- |
| `src/web/master*.cc`, `web/src/state/api.ts` | The existing web API covers files, library, terminals, receipts, previews and sessions; `SessionHost` alone does not. |
| `MasterMain` in `src/web/master.cc` | `~/.uagent/web/master.lock` and authenticated `discovery.json` identify one running web host; do not create alternate state. |
| `session_supervisor.cc`, `session_worker.cc` | Binary replacement can currently recycle a busy worker through `close`; atomic installation alone cannot make upgrades safe. |
| `use-host.ts`, `snapshot-store.ts` | Persistent device metadata and in-memory snapshots exist; persistent offline transcripts do not. |
| `sidebar.tsx`, `pins.tsx`, `folding.ts`, `app.tsx` | Grouping, keyboard order, whole-list pin writes, folding and one desktop/drawer sidebar already exist. |
| `install.sh`, `CMakeLists.txt`, CI packaging | Three platform builds exist; Intel macOS and the complete remote bundle need release work. Current minimums include glibc 2.38 and macOS 15. |

A streaming proxy, process survival, payload sizes and platform compatibility
have not been validated at runtime. The choices below are the implementation
contract; stage 1 proves the transport and lifecycle assumptions first.

## User experience

### First connection

1. **Hosts +** and **Open workspace → Remote machine** enter one shared flow.
2. Enter `user@machine` or select an SSH alias.
3. Verify access and host identity. Unknown keys require explicit trust;
   changed keys stop the flow with an actionable explanation.
4. Probe platform and authenticated web-host discovery. Reuse a matching
   running host; do not bypass an occupied lock with another state directory.
5. If no host runs, copy only its verified payload, install at the fixed
   managed path and start detached from SSH.
6. Check exact runtime build/API identity and attach through the existing
   authenticated API. A matching installation skips transfer.
7. Reuse remote provider settings or open Settings targeting that host.
8. Browse its directories, choose a folder and open the normal workspace.

Connecting, Checking installation, Installing, Starting and Syncing are progress
details within Connecting, not a second persistent bootstrap state machine.

For missing key access, suggest `ssh-copy-id user@machine` and verification
with `ssh user@machine`, preserving actual alias/port configuration. Distinguish
network, authentication, trust, platform, transfer, version and startup errors.
Support interactive encrypted-key unlocking; batch failure does not prove keys
are missing. Never collect private keys in the UI.

### Sidebar and pinning

```text
Pinned                         collapsible
  Conversation                 workspace · host when needed
Recent                         collapsible, initially closed
  Conversation shortcut        workspace · host
Local                          collapsible host group
  Workspace or folder
    Conversation
devbox
  atlas                        workspace icon
    Conversation
  logs                         folder icon
build-mac
  releases
─────────────────────────────
Hosts                       +
  Local             Connected
  devbox            Connected
  build-mac      Disconnected
─────────────────────────────
Refresh / Settings             existing footer or selection actions
```

- Group host → workspace/folder → conversations, without per-place host
  badges. Keep existing icons and place actions.
- Preserve conversation, folder and terminal pins. Run `livePins` and pin
  mutations against each host's own catalogue/list. Never send aggregated
  pins to a whole-list replacement command. Combine only for rendering, in
  stable host order and each host's pin order.
- Pinned conversations appear once in the main list, as today. Missing offline
  catalogues are not deletion: retain pins and disable unavailable writes
  rather than invent an offline mutation queue.
- Recent derives ten newest unpinned top-level conversations from catalogue
  timestamps, without visit tracking. Its rows are navigation shortcuts,
  outside range selection and Alt+↑/↓ order; native Tab/Enter still works.
  They have no bulk-selection checkbox. Their source rows remain under the
  host. No deduplication algorithm is needed.
- Extend the existing collapsed set with host-qualified keys. Reuse the same
  `remembered` helper with an opened set for default-closed Recent. Preserve
  explicit folds; mark a folded active place instead of forcing it open.
- Hosts is its own bottom region above the footer/selection bar. Show every
  saved host and Local with square marks and explicit Connected, Connecting,
  Reconnecting or Disconnected text. Keep it collapsible and cap list height
  on phones, reusing existing scrolling so navigation/actions stay reachable.
- Host details show address, status/error and Connect/Reconnect, Disconnect
  and Open folder as appropriate. Refresh remains a current-host catalogue
  refresh; it is not an implicit SSH connection or recovery action.

### Non-sidebar pages

One current-host reference follows the selected conversation/place and defaults
to Local. Preserve it in host-qualified navigation across reload/back. Library,
Scheduled, Search, folder overview and execution Settings use it and show the
host in their headers. A small host selector reuses the saved-host list; do
not aggregate these pages across machines.

Appearance, device pairing and serving-host connection management keep their
existing ownership. Providers, tools, schedules and workspace policy target
the execution host. Unavailable targets show Reconnect, never Local results.

### Connect and recovery

Only an explicit connect request starts a disconnected SSH connection: initial
onboarding, a deliberate conversation/place click, or Connect/Reconnect.
Clicking a saved remote conversation issues that request automatically; no
separate Connect step is needed. URL restoration, snapshot reads, catalogue
refresh, group expansion and UI-stream resume never issue it.

The manager owns one retry policy: initially five jittered retries around
1, 2, 4, 8 and 15 seconds after unexpected loss. Bound attempts and use OpenSSH
connect timeout and server-alive options to detect dead connections. Keep
limits centralized; do not add a custom heartbeat loop.

| Situation | Behavior |
| --- | --- |
| Unexpected SSH loss | Reconnecting, retained available output, bounded retries. |
| Between attempts | Retry now in the host menu and active conversation resets the budget. |
| Attempt in flight | Disable duplicate retry controls; concurrent callers join the operation. |
| Budget exhausted | Disconnected, useful reason and Reconnect; no background retry loop. |
| Intentional Disconnect | Cancel timers/attempts and detach this gateway's transport. Persist intent across UI reloads; remote work continues. |
| Trust/authentication/platform/version failure | Stop retries and offer the specific corrective action. |
| Reconnect on a healthy host | Check/reuse the transport without restarting remote work. |

After intentional Disconnect or budget exhaustion, a new explicit user request
is required. Browser online/resume events retain existing UI-hop recovery;
they never replenish SSH retry budgets. One waking phone cannot re-arm retries
forever. Bootstrap/retry requests share the same connection owner.

Derive availability from both hops: a remote action requires the browser's
serving-host connection and that remote host to be ready. During a UI-hop
outage, remote rows show that outage with last remote state in details, not a
misleading live Connected. SSH failure affects that host; UI/gateway failure
makes every remote action unavailable on that device.

Recover host-specific receipts/cursors/checkpoints before enabling writes.
Never replay tool execution or uncertain submissions. Retain drafts and show
unresolved outcomes honestly. Use a short banner grace period for transient
loss without hiding host state. Disconnect details explain its scope: every
place/client using this gateway's transport detaches; sessions are not stopped.

### Offline and mobile

- Namespace today's persisted catalogue, spaces, drafts, read/unread and
  outgoing metadata by host. Do not introduce disk transcript/file caches.
  Keep in-memory snapshots visible during a live disconnect.
- After offline reload, show saved rows, draft, host status and Reconnect.
  Only show a transcript already held in memory; do not promise cached output
  after reload or infer a task's current state from old output.
- Reuse the existing drawer/model/actions. Conversation selection closes the
  drawer. Pin/menu/host controls work by tap with roughly 44px targets.
- SSH runs on the reachable serving host, not the phone. Local means that
  machine; show its actual name/address in details.
- Keep square controls/status marks and responsive rules in shared styling.
  Retain compact user messages with timestamp/menu in a separate footer line.

## Architecture and state ownership

```text
Desktop / phone web UI
       |
Serving uAgent: authentication, saved hosts, routing
       |                              |
Existing Local API             /api/hosts/<id>/... proxy
                                      |
                              one OpenSSH transport
                                      |
                         existing remote web-host API
                                      |
                           SessionHost and runtimes
```

### Existing API over SSH

Prefix routes centrally in `web/src/state/api.ts` with the host reference;
keep Local's existing route. The serving host authenticates the browser and
authorizes the selected saved host. Forward HTTP, SSE and WebSockets through
one SSH forward with existing remote pairing/device authorization. Keep remote
credentials on the gateway; never expose them or discovery secrets to browsers,
files/previews or substitute Local cookies for remote authorization.

Prefer a private Unix-socket API listener for a new managed backend. For an
existing web host, reuse its discovered authenticated loopback endpoint, with
the same handlers. Prove the current OpenSSH/cpp-httplib path supports these
listeners and streaming before expanding it. Do not invent stdio framing.

Preserve SSE flushing/cancellation, WebSocket upgrades/close, upload/download
streaming, preview links and origin checks. Rewrite only necessary host URL
and cookie scope; this is not an arbitrary URL proxy. Reuse existing request,
stream and body limits. Verify bulk traffic does not starve controls before
adding extra scheduling, multiplexing or SSH transports.

Maintain one browser catalogue, snapshot store and event cursor per connected
host through existing reducers/recovery logic. Remote events keep their origin
epoch/cursor and never enter Local's replay log. Route browser viewers,
terminals, assets, settings and relative links through the same API context.

### Identity and storage

- Workspace identity is verified host ID plus remotely canonicalized path.
  Host-qualify sessions, terminals, requests, links and device caches so equal
  IDs or paths on different hosts never collide.
- Save destinations, verified host identity and explicit disconnect intent
  in private `~/.uagent/web/hosts.json`, using existing private atomic file
  helpers. Live transport/retry state belongs only to the connection manager.
- Alias changes retain verified identity. A new backend ID is a new host;
  do not auto-merge old references or build a reconciliation service.
  Changed SSH keys still require explicit trust resolution.
- Pins stay on their execution host. Folding stays device-local. Provider
  settings and sandbox/workspace policy stay execution-host-owned.
- Files stay remote; browser uploads/downloads use the existing API. No
  filesystem mount, background sync or new machine-to-machine transfer layer.

### Existing hosts and process lifetime

The backend is the remote user's one web host using `~/.uagent/web` and its
lock. Authenticate discovery and reuse a matching build. An incompatible or
unverifiable lock owner yields a useful error, never another port/state tree.

If no host runs, start the managed executable detached from SSH with private
logs and existing worker lifetime handling. This is the only new survival
mechanism in v1: do not generate remote systemd/launchd units. Existing
user-managed services retain ownership. Test SSH/network-disconnect survival
on Linux/macOS, including child work. Detachment is not proof of survival;
report logout restrictions and do not silently alter system policy. Reboot
startup is deferred.

### Exact builds and safe updates

Require exact runtime build identity between gateway and backend, independent
of payload flavor, plus existing API/session protocol checks. Record it in
manifest/discovery: the same public version string alone does not identify
an unpublished build. Do not introduce cross-version negotiation/adapters.

Use one managed path, `~/.uagent/remote/uagent`, replaced by verified atomic
rename. Before replacing/re-execing a running managed host, prove it is idle
through a supported API: no active turn, pending approval, supervised work or
terminal that would be interrupted. Otherwise show **Update pending; remote
work continues**. If an old API cannot establish safety, or another build is
user-managed, show manual-update guidance without stopping it or replacing its
service binary. No side-by-side remote version manager or cleanup service.
The idle decision and update admission must be serialized on the remote host:
block new work until re-exec or failed-update recovery releases the gate.
A read-only idle check followed by an unrelated SSH replacement is insufficient.

For same-protocol replacement, fix the shared stale-worker recycle gate to
defer while busy and reuse restart-pending state. Idle includes the end of
approvals and supervised work, not merely an absence of streamed text. Prove
reattachment/re-exec preserves work; this is an actual required lifecycle fix.

If the session protocol changed, refuse attachment to old busy workers. Show
version mismatch/update pending and retain drafts; offer the matching installed
client or retry after work finishes. No automatic stop or pretend compatible
live view. A newer client may temporarily lose attachment, never terminate work.
Repair/update actions remain separate from deleting conversations or files.

## Build, bundle and installation

### Small shared build

Build the existing executable with an empty web-asset table and a headless
web-host entry path. Preserve Master, terminals and required runtime services;
do not first split `uagent_app` just to remove linked UI classes. Measure
compressed/installed bytes, idle memory/CPU and startup before further
extraction. Browser executables stay optional.

Audit skills, certificates, notices and defaults. Reuse resource lookup;
embed small mandatory resources or include them in the native payload archive.
Removing frontend assets must not remove required runtime resources. Bootstrap
must not download dependencies on the remote host.

### Compatibility policy

Retain current declared floors for v1: glibc 2.38 on Linux and macOS 15.
Do not turn this feature into a musl/toolchain migration. Build each native
CPU target against those floors; bundle/link pinned non-system dependencies
where required instead of relying on Homebrew or newer-host libraries.
Verify DNS, TLS, subprocess, PTY and sandbox behavior on clean minimum systems.
If the floors cannot be met, fix artifacts or revise the advertised minimum
before release, never silently raise it. Older systems get an unsupported
platform result. Native Intel macOS proof is required; Rosetta is supplementary.

### Complete four-target bundle

```text
<prefix>/bin/uagent
<prefix>/share/uagent/remote/<build-id>/
    manifest.json
    linux-x86_64.tar.gz
    linux-arm64.tar.gz
    macos-x86_64.tar.gz
    macos-arm64.tar.gz
```

Each archive contains its executable and only mandatory runtime resources.
The manifest records exact runtime build ID, target/minimums, contents, sizes
and hashes. No protocol-compatibility range is promised. Copy/extract only the
selected target and keep the remaining payloads compressed locally.

- Release installation obtains the main application and one matching complete
  bundle using existing verified release delivery. Switch bundles only after
  complete integrity checks; interrupted installation preserves the old one.
- Source installation obtains an exact-build published bundle or an explicitly
  supplied complete CI/developer bundle. Do not require local cross-compilation.
  With no complete matching bundle, fail remote-ready installation clearly;
  a local-only developer build cannot report remote support ready.
- `./install.sh` in a checkout still delivers all four payloads for a finished
  install. The review's own-target-only fallback is not adopted because it
  contradicts the required ready-to-copy bundle.
- CI waits for four native target artifacts. Extend package-content tests,
  release artifact counts, custom-prefix and container checks.
- Serialize bootstrap, stage/extract privately, verify target/hash/resources,
  test startup and atomically publish. Do not edit shell startup, provider
  settings or the remote user's CLI.

Container installation must include OpenSSH and the complete bundle. Document
explicit read-only SSH configuration/key mounts or an agent socket, writable
private known-hosts storage and ownership. Do not assume the desktop's SSH
agent is available or automatically forward it to the remote host.

## Credentials and optional capabilities

- Remote provider credentials keep work independent of the client. Never
  silently copy local keys, browser profiles, MCP secrets or full config.
- Preserve remote sandbox and authoritative approval checks. Missing remote
  Git/browser/MCP capabilities report their state and never execute locally.
- Keep sockets/state private and authenticate loopback reuse; forwarding
  does not authorize arbitrary users of the gateway machine.
- Remote approvals arrive through the proxied event stream. Preserve existing
  Local push; remote push aggregation is deferred, not advertised as v1 support.

## Implementation stages

### 1. Prove the shared API and lifecycle path

Prototype authenticated proxying to an existing host: ordinary requests, SSE,
terminal WebSocket, upload/download and static previews, including cancellation,
URLs and origin checks. Prove Unix-socket/discovered-loopback forwarding,
exact-build discovery, detach survival and the idle/recycle gate. Measure the
asset-free build on the declared floors. Resolve failed feasibility gates
before expanding routing; do not add another protocol as a default fallback.

### 2. Add host routing and SSH lifecycle

Implement the private host registry, explicit connect commands, joined
bootstrap, existing-host reuse and bounded retry/manual recovery. Namespace
API calls, caches, cursors, links and current-host navigation. Route existing
features, execution Settings and per-host pin writes. Preserve Local behavior
and derive both-hop availability without falling back to Local.

### 3. Apply the shared sidebar and mobile behavior

Extend existing grouping/folding/rows for Pinned, Recent shortcuts and host
groups. Add the bottom Hosts region and shared connection flow/menus. Keep
selection, Refresh and Settings reachable; remove replaced single-host paths.
Use one model/component set for desktop and the mobile drawer.

### 4. Package and verify the full release

Build all four targets, assemble the exact-build bundle and update release,
source/container installation and package tests. Run the checks below and
record real size/latency/platform results. Local tests, native platform proof,
hosted CI, published artifacts and deployed proof are separate claims.

## Acceptance checks

- Four clean native target installs, minimum systems, cross-OS connections,
  prefixes and container credentials. Bootstrap works with release-download
  access disabled; model-endpoint access is a separate requirement. Check
  copied artifacts' library/resource lookup and applicable signing/startup.
- Reuse an existing matching host with its pins/drafts/state, without a second
  state tree. Reject mismatches/unverifiable discovery without affecting work.
- Concurrent opens, two clients and rapid retries share one operation per
  host. Disconnect survives reload, URL restoration and another device.
  Exhausted retry/auth budgets never re-arm from browser online/resume.
- Drops before send, after send/before receipt, during output/approval: recover
  per-host cursors/gaps and receipts without replaying uncertain execution.
- Busy binary/protocol upgrades never cancel work. Exercise idle restart,
  pending approval, terminal/child work and independent versus managed hosts.
- Every existing feature, relative asset URL and execution setting targets
  the selected host; equal paths/IDs across hosts do not collide.
- Whole-list pin writes never contain another host's pins; offline pins survive.
  Recent shortcuts do not enter range/Alt order. Persisted folds win.
- Desktop and 320/390px Chromium/WebKit journeys: touch/keyboard, footer during
  selection, many-host scrolling, error/retry states and compact messages.
  Offline reload does not pretend a disk transcript exists.
- SSH/network loss and laptop sleep preserve remote work. Separately report
  logout/reboot restrictions. Bulk/noisy traffic must not stall other hosts;
  respect existing request/stream/queue bounds.
- Measure compressed/installed size, idle memory/CPU, bootstrap/warm reconnect
  and event latency. No per-operation SSH startup or directory polling loop.

Use focused ownership/projection/recovery regressions and a small real browser
journey, not duplicate desktop/mobile logic or markup snapshots. Remove replaced
paths and measure costs/LoC rather than promise speculative reductions.

## Deferred scope

CLI `--remote`, dev-server port forwarding/phone proxy, direct host-to-host
transfers, persistent offline transcripts/files, cross-host search/library/
schedule aggregation, cross-host pin reordering, remote push aggregation,
backend-identity reconciliation, remote service installation, reboot startup,
password/key provisioning, Windows/32-bit, filesystem mounts/sync, cross-host
delegation, local credential relay, local-browser fallback, older-libc support
and automatic migration of complete local configuration.
