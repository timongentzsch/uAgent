# Container

This page runs the Compose appliance in `deploy/`: the web host, the agent,
its terminal and its Chrome in one container, behind a second container that
owns the network. What the container does and does not bound is policy and
is stated in [Security](../../SECURITY.md#the-container); how the browser
works is in [Browser](../architecture/browser.md).

## Start it

```sh
docker compose -f deploy/compose.yaml up --build -d    # from a checkout
docker compose -f deploy/compose.yaml logs uagent
docker compose -f deploy/compose.yaml exec uagent uagent setup   # keys, model
docker compose -f deploy/compose.yaml exec uagent uagent --web   # a pairing link
```

From a release, `install.sh` with `UAGENT_INSTALL=container` downloads that
release's `compose.yaml` and `seccomp-chrome.json` into
`~/.local/share/uagent/container/deploy`, writes `UAGENT_VERSION` and
`UAGENT_WORKSPACES` to `.env` there, runs `up -d --no-build --pull always`
on `ghcr.io/timongentzsch/uagent:<version>` (amd64, arm64), waits up to 60 s,
then runs `uagent setup` and `uagent --web`. It writes `<prefix>/bin/uagent`
(`~/.local/bin/uagent`), which runs the container's `uagent` with the
arguments given: `uagent setup`, `uagent --web` and the rest work as on a
native install. It passes this terminal when there is one (`exec -T` when
not) and says how to start the container when it is not running. A native
µAgent in the same prefix (`share/uagent/install` exists) keeps the name and
the installer says the container's command instead; a native install over a
container one replaces the command, and the container stays reachable with
`docker compose -f ~/.local/share/uagent/container/deploy/compose.yaml exec
uagent uagent`. The Compose project is named
`uagent` either way, so both copies share one volume. Use Docker Engine 28
or newer: [older engines can expose a loopback-published port to nearby hosts](https://docs.docker.com/engine/network/port-publishing/).

## The two services

| Service | Runs | Holds |
| --- | --- | --- |
| `network` | `deploy/firewall.sh` as root (`0:0`) | `NET_ADMIN` only; publishes the port; owns the network namespace |
| `uagent` | `deploy/start.sh` as UID 10001 (`agent`) under `tini` | `SYS_CHROOT` only; `network_mode: service:network` |

The rules need root and `NET_ADMIN`, so they live where the agent cannot
reach them. `uagent` waits for `network` to be healthy (its `nft` table
exists) and restarts with it. Inside, the host binds `0.0.0.0`
(`UAGENT_WEB_BIND`); Compose publishes the port on the Docker host's
`127.0.0.1` only.

## Update it

Rerun the installer with `UAGENT_INSTALL=container`; it keeps the
workspaces folder from `.env`, writes the new version and pulls. From a
checkout: `git pull` then `up --build -d`. Settings → About shows the
installer command and no **Update** button. The data volume is kept.

## Variables

Compose reads `.env` beside `compose.yaml` for substitution. A file
`.env.appliance` one directory up (the checkout root, or
`~/.local/share/uagent/container/`) is passed whole into the `uagent`
container's environment: settings such as `UAGENT_WEB_ORIGIN`, provider
keys, and `TZ` and `LANGUAGE` (`Europe/Berlin`, `de`), which the browser
service passes to Chrome so pages see that clock and language rather than
UTC and English. Keep the file private.

| Variable | Default | Effect |
| --- | --- | --- |
| `UAGENT_VERSION` | `latest` | image tag of both services |
| `UAGENT_WEB_PORT` | `8080` | published loopback port and listener port |
| `UAGENT_WORKSPACES` | `../workspaces` | host directory mounted at `/workspaces` |
| `UAGENT_NETWORK` | `private` | `open` loads no rules |
| `UAGENT_NETWORK_ALLOW` | empty | private destinations the agent may reach |
| `UAGENT_CHROME` | `auto` | `off` starts no Chrome updater |
| `UAGENT_PIDS_LIMIT` | `4096` | processes and threads of `uagent` |
| `UAGENT_MEM_LIMIT` | `0` (none) | memory limit, such as `4g`; size it with `deploy/footprint.sh` |
| `UAGENT_CPUS` | `0` (none) | CPU limit, such as `4` |
| `UAGENT_SHM_SIZE` | `256m` | `/dev/shm`, used by Chrome |
| `UAGENT_TMPFS_SIZE` | `512m` | `/tmp` tmpfs |

## Volumes

| Mount | Content | Persists |
| --- | --- | --- |
| `uagent-data:/data` | `/data/agent` (`HOME`: `~/.uagent`, `~/.ssh`), `/data/browser` (`UAGENT_BROWSER_DATA`: profiles, `chrome/`) | until `docker compose down -v` |
| `${UAGENT_WORKSPACES}:/workspaces` | the folders the agent works in | on the host |
| tmpfs `/tmp` | runtime sockets, scratch | no |

On Linux the installer gives UID 10001 `rwx` ACLs on the workspaces folder
with `setfacl`. Back up the whole volume to keep logins and keys. SSH hosts
are configured in `/data/agent/.ssh`; test one with
`docker compose -f deploy/compose.yaml exec uagent ssh -o BatchMode=yes ALIAS true`.

## Network rules

`deploy/firewall.sh` loads an `inet uagent` nftables table with an output
chain: loopback and established traffic pass; entries from
`UAGENT_NETWORK_ALLOW` and Docker's upstream DNS servers (port 53) are
accepted; then the bridge gateway, `0.0.0.0/8`, `10.0.0.0/8`,
`100.64.0.0/10`, `169.254.0.0/16`, `172.16.0.0/12`, `192.168.0.0/16`,
`224.0.0.0/3`, `fc00::/7`, `fe80::/10` and `ff00::/8` are rejected with
*admin-prohibited*, so a refused connection fails at once. Public addresses
pass. This covers the agent's commands, terminal and Chrome alike.

`UAGENT_NETWORK_ALLOW` takes entries separated by spaces or commas:

| Entry | Accepts |
| --- | --- |
| `192.168.1.0/24` | that IPv4 CIDR, any port |
| `nas.lan` | every IPv4 address `getent ahostsv4` returns, any port |
| `192.168.1.20:22`, `llm:8000` | those addresses, that TCP port only |

Names resolve once when `network` starts; an unresolvable one stops it, and
`uagent` with it. A container named there must share a Docker network with
`network`. DNS goes to Docker's upstream resolver, which may be on your LAN.

```sh
deploy/check-network.sh                         # or: deploy/check-network.sh -f FILE
CHECK_LISTENER=192.168.1.5:80 deploy/check-network.sh
```

From inside `uagent` it checks that `https://example.com` answers, that
private addresses and `CHECK_LISTENER` (one known to listen) are refused,
that `CapEff` is 0, `NoNewPrivs` is 1, the ruleset is out of reach and the
UID is 10001. It prints the first `FAIL:` line or `network: public
reachable, private refused, rules out of the agent's reach`.

## Hardening

Both services: `read_only: true`, `no-new-privileges:true`, `cap_drop: ALL`.
`network` adds `NET_ADMIN` and `pids_limit: 64`; `uagent` adds `SYS_CHROOT`
(Chrome's sandbox calls `chroot` in its own user namespace), the limits and
tmpfs above, logs of at most 3 × 10 MB, and `deploy/seccomp-chrome.json`:
Docker's default profile with `clone` and `unshare` allowed, for every
process in the container. No Docker socket is mounted.

## Chrome on the data volume

The image holds Chrome's system libraries but no Chrome. `chrome-update`
(`deploy/chrome-update.sh`) fetches Google's stable `.deb` to
`/data/browser/chrome/<version>`, accepting it only when `gpgv` verifies the
repository `Release` against the key in the image
(`deploy/google-chrome.asc`) and the `Packages` list and package match their
SHA-256. It switches `chrome/current` by rename and keeps the version a
running Chrome uses. It runs at start, then daily, or after 10 min when a
check failed; a failure keeps the installed version. `chrome/status` holds
`downloading <v>`, `ready <v>` or `failed <reason>`; About shows it with the
time it was written; **Check now** runs `chrome-update` once.

## The agent's browser without Docker

On Linux the web host runs the same browser natively:

1. Install `google-chrome-stable`, `Xtigervnc`, `xauth` and `xcompmgr`
   (without it Chrome's popups have black margins) in a directory
   of the service's fixed `PATH`
   (`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`).
2. Save `web.browserData` as a user setting, so every runtime sees it:
   `/config user web.browserData=/home/you/.uagent/browser`. Keep the path
   short and outside the sandbox's writable roots.
3. Optionally, `sudo install -D -m 0644 deploy/chrome-policy.json
   /etc/opt/chrome/policies/managed/uagent.json`; it applies to every Chrome
   on the machine.
4. Restart the web host; `<data>/service.sock` shows the service runs.

Chrome then runs as you, without the container's UID, seccomp profile and
network rules.

## When it goes wrong

| Symptom | Cause | Do |
| --- | --- | --- |
| `uagent` never starts; `network` unhealthy | rules did not load, or `network: cannot resolve <entry>` | `docker compose -f deploy/compose.yaml logs network`; fix `UAGENT_NETWORK_ALLOW` |
| `Chrome <v> is still downloading; try again in a minute` | first download running | wait; `chrome/status` shows progress |
| `Chrome could not be downloaded: <reason>` | first fetch or verification failed | check network rules and the reason; About → **Check now** |
| `Chrome exited during startup; check its sandbox and container logs` | the host forbids unprivileged user namespaces, or the seccomp profile is missing | allow user namespaces on the Docker host |
| `browser data directory must be private and have a short path` (native) | mode, owner or socket path length | `chmod 700`, a shorter path |
| a LAN host does not answer | private addresses are refused | `UAGENT_NETWORK_ALLOW` |
| `µAgent <v> publishes no container image; installing here instead.` | that release has no compose files | pick another `UAGENT_VERSION` |
