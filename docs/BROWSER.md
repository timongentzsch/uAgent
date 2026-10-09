# Browser appliance

The browser the agent can drive and you can watch. For the web interface
itself see [Web interface](WEB.md).

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

## Agent and human control

The agent's `browser` tool and the viewer drive the same visible tab. Each
agent action waits for the page to settle and returns the page as text: what
can be clicked or filled, numbered, and the page's text. The agent acts on a
number, reads more of a long page or only the lines it searches for, and
asks for a screenshot where text does not describe the page. A tab the
action opens becomes the active tab, and the agent can open one of its own. A page that looks like a bot
check or rate limit is flagged as a suspected block, so the agent can hand
off or try another source.

| You want to | Do this |
| --- | --- |
| Watch the agent | The globe button in the conversation header opens the live screen, read-only |
| Drive yourself | **Take over** pauses the agent and enables input; **Done** hands back |
| Answer the agent's request | **Open browser** on its card opens the viewer already in your control; **Done** returns you to the chat |
| Free Chrome's memory | **Stop browser** in the status menu, shown while Chrome runs unused |

- The host serves the screen to one viewer at a time.
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

## Profiles and saved logins

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

## Without Docker

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
