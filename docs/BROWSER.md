# The agent's browser

The web host can run a real Google Chrome that the agent drives and that you
can watch, take over and sign in to from any paired device. For the web
interface itself, see [Web interface](WEB.md).

This browser is the user's own: one persistent, signed-in profile that one
conversation drives at a time. Where it is not set up (macOS, a terminal-only
install), the bundled `browser-use` skill gives the agent a throwaway headless
browser through `playwright-cli` instead, one per process and with no saved
logins; it also suits checking a page you are building.

The optional [Compose stack](../deploy/compose.yaml) builds one image with
the web host, Chrome Stable and TigerVNC; a Linux host with the same
programs installed runs it [without Docker](#without-docker).

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
- Folders go in `workspaces/`, mounted at `/workspaces`. On a Linux host,
  give UID 10001 read/write access to it.
- The `uagent-data` volume holds `/data/agent` (history, pairing, settings)
  and `/data/browser` (Chrome profiles). Back up the whole volume to keep
  every login.
- Chrome and Xvnc stay stopped until the browser is first used.
- Set `TZ` and `LANGUAGE` (for example `TZ=Europe/Berlin` and
  `LANGUAGE=de`) in `.env.appliance`: pages see that clock and language
  instead of the container's UTC and English. Chrome sets a profile's
  languages when it creates it.
- Chrome runs as a person's browser: pages do not see
  `navigator.webdriver`, WebGL renders in software, the agent's scripts run
  in a world of their own that the page cannot see, and text is typed key
  by key. Sites that turn away test browsers then serve the agent's
  searches and bookings; a bot check that still appears is handed to you
  (`request_human`), never solved by the agent.

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

## Agent and human control

The agent's `browser` tool and the viewer drive the same visible tab.

- Each agent action waits for the page to settle and returns it as text: the
  page's text and its numbered controls. A number keeps naming its element
  while the page stays, so several actions can come from one look. The agent
  can read more of a long page, search it for lines, or ask for a screenshot
  where text does not describe the page.
- Besides clicking and typing, the agent can right-click, double-click,
  drag (sliders, sortable lists, drag-and-drop), press key combinations such
  as Control+A, and go back, forward or reload.
- For debugging a page, the agent reads what its console logged and the
  errors it did not catch, and the tab's requests since the page began:
  method, status or failure, kind, address and size. The console is read
  with Chrome's Runtime events on for that moment only, since a page can
  notice them.
- A tab an action opens becomes the active tab, and the agent can open one of
  its own.
- A page that looks like a bot check or rate limit is flagged as a suspected
  block, so the agent can hand off or try another source.
- A page's alert, confirm or prompt is reported in place of the page until
  the agent answers it with Enter or Escape.
- A password field takes no typed text: the agent uses a saved login or asks
  you.
- The agent uploads only files of the conversation's folder, and what a page
  downloads lands in that folder's `downloads`. A PDF link downloads there
  too (the managed policy), rather than opening in Chrome's viewer. A
  download that ends after the action that started it is reported with the
  agent's next browser result.

| You want to | Do this |
| --- | --- |
| Watch the agent | The globe button in the conversation header opens the live screen, read-only |
| Drive yourself | **Take over** pauses the agent and enables input; **Done** hands back |
| Answer the agent's request | **Open browser** on its card opens the viewer already in your control; **Done** returns you to the chat |
| Free Chrome's memory | **Stop browser** in the status menu, shown while Chrome runs unused |

- The host serves one viewer per browser instance at a time.
- While you drive, the owning agent waits for handback before its next
  action. Other browser instances can keep working.
- The agent calls `request_human` for a login Chrome has not saved, MFA, a
  captcha or bot check, or a payment confirmation, and the conversation shows
  its reason. Only the device that took control can finish it; closing the
  viewer keeps the agent waiting. A request whose conversation no longer runs
  is dropped.
- Without a pending request or profile sign-in, closing the viewer hands
  control back like **Done**.
- One conversation uses the browser at a time. It is free again when that
  conversation's turn ends, after three minutes without a browser action, or
  when the agent calls `release`; a dead session's runtime lease also frees
  it. Competing calls wait in a bounded first-in, first-out queue, with their
  position shown in the live activity label and the browser status menu.
  Cancelling a waiting call removes it without performing its action.
- When a turn on the shared browser ends, only the tab the agent was on
  stays open.
- Chrome stops after 15 minutes without browser work, unless you control it
  or the agent waits for you, and starts again on the next action. Watching
  alone does not keep it running.

## Parallel research

In the browser status menu, **Browser scheduling** selects the shared browser
(the default) or 1, 2 or 4 isolated research browsers. The choice is saved in
the browser data directory's `scheduler.json`. Start with two and measure the
host's memory and CPU before going higher.

- When enabled, new agent calls use the isolated pool. Each slot has its own
  Chrome process, profile, display, control channel and viewer socket.
- A conversation keeps its slot until it releases it or its turn ends. The
  profile is reset before another conversation gets the slot, so cookies,
  logins and page state are never shared between research conversations.
- Lowering the count lets current holders finish; new calls use the
  remaining capacity. Turning it off moves waiting work to the shared queue.
- `browser=shared` selects the persistent signed-in browser for a
  conversation that holds no slot; release a research slot first. All
  callers, including a lead agent and its background agents, use the same
  broker, so a lead agent should release its browser before waiting on a
  background agent that needs it.
- **Viewing** switches between instances and names the conversation that
  owns each. Take over, Done, profile controls and clipboard input apply only
  to the instance shown. A turn that ends while you hold its browser releases
  it at handback.
- A queued call waits at most until its tool deadline and the five-minute
  admission limit; an abandoned entry expires after 15 seconds without a
  poll. Actions carry a lease token, so a request from an earlier owner
  cannot reach a reassigned browser.

## Profiles and saved logins

A profile is one persistent Chrome login state, shared by the agent and the
viewer. In the status menu, **Chrome profile** selects one and **New profile**
creates another. Switching needs control (or a stopped Chrome) and restarts
Chrome in the chosen profile. No cookies are copied between profiles.

To sign in to sites: take over, then choose **Sign in to profile**. This
reopens the profile in ordinary Chrome without a debugging connection. Sign in
and complete MFA, then choose **Done** to reopen the same profile for the
agent. Chrome restores saved tabs; finish unsaved page edits first. No browser
security checks are disabled, and Google may still reject a sign-in from a
browser it does not support; see its
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

## Viewer controls

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

## Isolation

Use an isolated Docker host. The image runs the web host, agent workers and
browser service under one nonroot UID and is designed for one human with
several paired devices.

- The service exposes neither a CDP nor a VNC TCP port. The agent reaches
  Chrome over inherited pipes, the viewer over the authenticated web host.
- Chrome's sandbox needs a narrow [seccomp profile](../deploy/seccomp-chrome.json) that
  permits user namespace creation. Compose applies it without privileged mode
  and without disabling the sandbox. If the host disables unprivileged user
  namespaces, takeover fails and the browser stays stopped.
- With the command sandbox on, agent commands cannot read the browser data
  directory (saved logins, cookies) or connect to its sockets, and the file
  tools refuse it. `run(sandbox=false)` and `sandbox.level=off` lift that, as
  they lift the sandbox; yolo does not. See
  [SECURITY.md](../SECURITY.md#sandboxing).
- Screenshots draw every password field as dots, including one a site's
  reveal toggle switched to text; autofill still works. Fields in cross-site
  iframes and shadow roots are not masked.
- `back` and `forward` refuse history entries that are not HTTP(S), like
  `open`.
- `open` with `path` shows a self-contained `.html` or `.svg` file the agent
  wrote, so it needs no local server to look at its own page. The file's
  markup is written into a blank page: it has no `file://` origin, cannot
  load local files or its own relative links, and the profile directory is
  refused as a path. `file://` URLs stay refused.
- The [managed policy](../deploy/chrome-policy.json) blocks Chrome's local
  password and autofill pages, `view-source:` and DevTools. DevTools is
  blocked by its `devtools://` URLs, because `DeveloperToolsAvailability`
  would also refuse the browser service's own control pipe ("Not allowed",
  Chrome 155).
- On Linux before 7.1 (Landlock ABI 9) the sockets stay reachable: the agent
  can drive the browser, which it may do anyway, but with the policy
  installed it reaches neither DevTools nor the password pages.

## Without Docker

A native web host runs the same browser; Docker is not needed. It works on
Linux only: the host starts `google-chrome-stable`, `Xtigervnc` and `xauth`,
and Chrome runs on a virtual X display.

1. Install Google Chrome Stable, TigerVNC (the `Xtigervnc` binary) and
   `xauth`. The browser service starts them with the fixed `PATH`
   `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, so they
   must be in one of those directories.
2. Set `UAGENT_BROWSER_DATA` for the web host to an absolute directory only
   you can use, such as `~/.uagent/browser`. Set it in the host's
   environment, or save it as `web.browserData` (`/config user
   web.browserData=...` in a terminal session). The host creates the
   directory with mode `0700` and refuses one that others can reach; the
   path must be short enough for a Unix socket. Keep it outside the
   sandbox's writable roots.
3. Optional, once: install the managed policy, which the image does for you.

```sh
sudo install -D -m 0644 deploy/chrome-policy.json \
  /etc/opt/chrome/policies/managed/uagent.json
```

A systemd user unit, `~/.config/systemd/user/uagent-web.service`, with
placeholders for your user and tailnet address (see [Web interface](WEB.md)
for the origin):

```ini
[Unit]
Description=uAgent web host

[Service]
Environment=UAGENT_BROWSER_DATA=/home/USER/.uagent/browser
ExecStart=/usr/local/bin/uagent --web --web-port 18080 --web-origin http://TAILNET_IP:18080
Restart=on-failure

[Install]
WantedBy=default.target
```

Start it with `systemctl --user enable --now uagent-web`. To check it, look
for `service.sock` in the data directory (the host started the browser
service) and for the globe button in a web conversation's header. A
top-level session gets the `browser` tool once the service answers; a
terminal or headless session needs `UAGENT_BROWSER_DATA` in its own settings
or environment too. Chrome stays stopped until the agent or you first use
it. The data directory then holds `service.sock`, `Xauthority` and the
Chrome `profile/`.

What the container adds that a native setup lacks:

- Chrome runs as your own user, with your file access, next to the web host
  and the agent's commands. The image uses a separate nonroot UID, a seccomp
  profile for Chrome's sandbox and a container boundary.
- The managed policy applies only if you install it as above, and then to
  every Chrome on that machine, including your own.
- The profiles hold your saved logins. The command sandbox keeps agent
  commands away from them in both setups; protect and back them up
  accordingly.
