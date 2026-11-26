# Remote access

This page reaches the web host from other devices and connects SSH
execution hosts. The mechanisms are in [Web host](../architecture/web-host.md)
and [Execution gateway](../architecture/execution-gateway.md); what a paired
device and a remote account may do is in [Security](../../SECURITY.md).

## Pair a device

```sh
uagent --web
```

```text
uagent web: http://127.0.0.1:8080
Pairing code (single use, 5 minutes): <code>
Or open: http://127.0.0.1:8080/#pair=<code>
```

The host listens on `127.0.0.1:<port>` (`--web-port`, `web.port`). Running
`uagent --web` again while a host runs prints a fresh code from that host
and exits; a new code replaces the old one, and the running host keeps its
port and origin whatever flags the second call carries. A paired device
holds an `HttpOnly`, `SameSite=Strict` cookie for 30 days, not extended by
use, after which it pairs again. At most 16 devices are paired
(`kWebDeviceLimit`); `POST /api/auth` then answers 409. Revoke a device in
Settings → Host and devices → Paired devices, or **Log out this device**; its event
streams end at their next poll and every preview link is dropped.

## Give the host its public origin

```sh
uagent --web --web-origin https://agent.example.com
```

`--web-origin` (`web.origin`, `UAGENT_WEB_ORIGIN`) must be a bare origin:
`https://host[:port]`, or `http://` on a literal IPv4 address in
`100.64.0.0/10`, with no path, query, user or fragment. Anything else stops
the host with `web origin must be bare HTTPS, or HTTP on a 100.64.0.0/10
tailnet address`. The host accepts exactly two origins: the configured one
and `http://127.0.0.1:<port>`. It picks the expected origin from the `Host`
header; a request whose `Host` is neither authority, or whose `Origin`
differs from the one its `Host` selects, gets 403 `invalid Host or Origin`.
Forwarding headers (`X-Forwarded-*`, `Forwarded`) are never read. A cookie
issued over an HTTPS origin carries `Secure`.

## Put a reverse proxy in front

The proxy terminates TLS for the origin and forwards to
`http://127.0.0.1:<port>`. It must:

- pass `Host` as the browser sent it, port included, and pass `Origin`
  unchanged;
- stream `GET /api/events` (Server-Sent Events) without buffering, and keep
  idle connections open longer than the 15 s heartbeat;
- forward WebSocket upgrades on `/api/terminals/<id>` and
  `/api/browser/viewer`.

Check it by pairing and opening a terminal. A 403 on every request means
`Host` is rewritten; updates that appear only on reload mean the event
stream is buffered.

## Tailnet over plain HTTP

```sh
uagent --web --web-port 18080 --web-origin http://100.64.0.10:18080
tailscale serve --bg --tcp=18080 tcp://127.0.0.1:18080
```

A TCP forward keeps `Host` intact. `web.bind` (`UAGENT_WEB_BIND`) accepts
only `127.0.0.1` or `0.0.0.0`; the second listens on every interface, as
the container does, and needs no forward. HTTP on a tailnet address is not a
secure context: the app cannot be installed, notifications stay off, and
the cookie has no `Secure` flag.

## SSH port forwarding

```sh
ssh -N -L 18080:127.0.0.1:18080 you@server
```

Then open `http://127.0.0.1:18080` locally. Loopback is a secure context, so
install and notifications work, and the host needs no `--web-origin`. The
local port must equal the host's port, because `Host` must read
`127.0.0.1:<port>`.

## Install as an app

Settings → This device → Install, where the browser offers it; it needs HTTPS or
loopback. On iOS: Safari → Share → Add to Home Screen, then pair the
installed app if its storage is separate.

## Notifications

Turn them on per device in Settings → This device → Notifications; both modes need
a secure context and the browser's permission.

| Mode | Works | Needs |
| --- | --- | --- |
| Background push | with the app closed | `web.pushContact` set to a `mailto:` or `https://` contact, libcurl with asynchronous DNS, a browser with push |
| Connected view | while the page is connected | nothing on the host |

Without background push the pane names the reason, such as `set
UAGENT_WEB_PUSH_CONTACT to a mailto or HTTPS contact to enable …`. A notice is raised
when a turn completes, an approval or question waits on you, an approval is
escalated, or a conversation fails. It carries the conversation id and, with
`web.notificationDetail` on, the conversation's title and a reason
(approval, question, error, finished); never a prompt, command or answer.
With it off the notice reads `µAgent needs your attention`. Push payloads
are encrypted (`aes128gcm`), sent only to FCM, Mozilla or Apple endpoints on
port 443, tried twice, and dropped after 60 s; delivery is best effort.
**Send test notification** checks it ([Settings](../interfaces/settings.md)).

## Connect an SSH execution host

An execution host runs a conversation's tools in one of its own folders
while the agent loop, keys and history stay on the main
([Execution gateway](../architecture/execution-gateway.md)).

The main needs OpenSSH and non-interactive access: the host key already in
`known_hosts` (`StrictHostKeyChecking=yes`) and a key that works with
`BatchMode=yes`. Check with `ssh ALIAS true`; install a key with
`ssh-copy-id`. The main also needs the remote bundle of its own build
([Install](install.md#build-and-install-from-source)). The remote host needs
Linux with glibc 2.38+ on x86_64 or ARM64, or macOS 15+ on ARM64, and on
Linux the shared libraries the binary links (libstdc++, libcurl, OpenSSL 3
libcrypto, zlib). It needs no compiler, root or network download.

Add a host with **Add host** under Remote hosts in the sidebar, as an SSH
alias or `user@machine` (`POST /api/hosts {action: "add", address}`; at
most 16). The main starts an SSH master, probes `uname`, copies the payload
for its build to `~/.uagent/remote/builds/<build id>/` unless
`/usr/local/lib/uagent/remote/<build id>/uagent` exists there, and marks the
host connected. A main update copies the new build on the next connection;
a build unused for 30 days is removed. SSH output goes to
`~/.uagent/web/ssh-<id>.log` on the main. Network failures retry after about
1, 2, 4, 8 and 15 s, then wait; **Reconnect** resets that budget. A restarted
main reconnects the hosts that were connected when it stopped.

| Event | What survives |
| --- | --- |
| SSH drops | history (on the main); remote commands and terminals keep running; the main reads results by receipt and repeats nothing |
| remote worker restarts | changed files; running handles report interrupted |
| main restarts | folder terminals reattach (`remote-terminals.json`); panel terminals end |
| idle | a worker with no connection and no children exits after 5 min; a folder shell no one views exits after 24 h without output |

## When it goes wrong

| Message | Cause and action |
| --- | --- |
| `invalid Host or Origin` (403) | proxy rewrites `Host`, wrong local port, or a URL other than the origin |
| `authentication required` (401) | cookie missing or expired, or device revoked; pair again |
| `web origin must be bare HTTPS, or HTTP on a 100.64.0.0/10 tailnet address` | fix `--web-origin` |
| `SSH host identity needs attention. …` | run `ssh ALIAS` on the main and verify the fingerprint |
| `SSH key access failed. …` | `ssh-add` an encrypted key, or `ssh-copy-id` |
| `SSH destination was not found. …` | fix the alias in `~/.ssh/config` |
| `Remote hosts require macOS 15+ or Linux with glibc 2.38+.` | unsupported remote |
| `A complete three-target remote bundle for this exact build is required.` | reinstall with the bundle ([Install](install.md)) |
| `Remote payload installation failed. …` | check the platform and `~/.uagent/remote/install.lock` on the remote |
