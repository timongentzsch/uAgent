# Browser

This page explains the agent's browser: the service process that owns
Chrome and its virtual display, how browsers are shared between
conversations, how a person watches and takes over, and what one `browser`
tool call does. Container hardening and network rules are in
[Container](../operations/container.md); what the profile exposes and to
whom is in [Security](../../SECURITY.md).

## Responsibility

`uagent --browser-service` owns every Chrome and Xvnc process and the
persistent profiles. Nothing else talks to Chrome: session runtimes send the
service bounded requests; the web host relays the display. Chrome is
driven over CDP on a pipe; no CDP or VNC TCP port exists. It needs Linux
with `google-chrome-stable`, `Xtigervnc`, `xauth` and `web.browserData`
(`UAGENT_BROWSER_DATA`) set; remote conversations have no browser tool.

## Processes and threads

| Process | Started by | Lives until |
| --- | --- | --- |
| `uagent --browser-service` | the web host, `StartService` | its owner pipe (fd 3) closes |
| `Xtigervnc`, one per instance in use | the service, on the first action or takeover | idle stop, `stop`, profile switch, service exit |
| `google-chrome-stable`, one per instance in use | the service, after Xvnc | as Xvnc |
| `chrome-update --loop` | `deploy/start.sh`, container only | the container |

The service gets a fixed environment: `PATH` of `<data>/chrome/bin`, where a
container's Chrome is fetched, then the system directories,
`HOME` and `UAGENT_BROWSER_DATA` set to the data directory, `LANG=C.UTF-8`,
and `TZ` and `LANGUAGE` passed through so pages see the user's clock and
language. It accepts on `<data>/service.sock` and answers each connection
on its own detached thread, at most 8 at once; a ninth connection is
closed unanswered. Once a minute it runs `Broker::Maintain`, which stops a
Chrome idle for `runtime.idleMinutes` (`UAGENT_IDLE_MINUTES`) unless a
person holds it or the agent waits for one. Watching is not use.

Xvnc draws a 1280 × 800 × 24 display at 15 frames per second, listens
only on `display.sock` (mode 0600, no TCP) and takes a fresh `Xauthority`
cookie. Chrome starts with `--remote-debugging-pipe` on fds 3 and 4, its
sandbox on, a 64 MiB disk cache, no component updates or back-forward
cache, and `--disable-blink-features=AutomationControlled` and
`--enable-unsafe-swiftshader`, so pages see neither `navigator.webdriver`
nor missing WebGL. The service's page scripts run in an isolated world,
`uagent`, that the page cannot see.

## Data and formats

Requests are a 4-byte big-endian length, then JSON, at most 12 MiB each
way; one request per connection; no request names a CDP method.
`browser::Request` waits 15 s by default, 1 s for probes and 30 s for
actions (`include/browser/browser.h`).

The data directory is created 0700 and refused if group or others can
reach it or the socket path is too long. It holds `service.sock`,
`display.sock`, `Xauthority`, `profile.lock` (`flock`), `profile/` (the
Default profile), `profiles/<id>/`, `profiles.json` (names, selection),
`handover.json` (a pending request for a person, kept across restarts),
`scheduler.json` (`parallel_capacity`, 0 to 4), and `p1/` to `p4/`, the
research instances, each laid out the same.
- `chrome/` in the container: `current` (a link to the version in use),
  `bin/google-chrome-stable`, `status` (`downloading`, `ready` or `failed`
  with a version or reason).

## Protocol

**Instances and leases.** `Broker` holds instance 0, the shared signed-in
browser, and research instances 1 to 4, of which `parallel_capacity` are
offered. A call first sends `acquire` with the session id and a request id,
every 250 ms, for up to the tool deadline or 5 min. Waiters form one FIFO
queue of at most 64; one not polled for 15 s is dropped. A grant returns a
`lease` token and an `instance_id`; every later operation carries the
token, and one from an earlier holder fails with `browser lease changed`.
A research instance's directory is deleted before each new holder, so no
cookies or logins pass between conversations, and its Chrome stops at
release. A holder loses the browser at `release` (sent by the tool or at
the end of every turn by `Application`), after 3 min without a call, or when
its runtime's lease file has no live owner. On release of instance 0 every
tab but the current one is closed. The next holder must `observe` or
`open` before it can act.

**Agent and person.** A runtime is in mode `idle`, `agent` or `human`. Any
paired device may watch; `takeover` from a device makes it the controller
and stops agent actions (`acquire` answers `Waiting for browser handback`).
`request_human` records an interaction id in `handover.json` and waits
until the person sends `done`; only the controlling device can. Closing the
viewer hands control back unless a request or a profile sign-in is
pending. `setup_profile` restarts Chrome without the debugging pipe for a
manual sign-in; `done` restarts it with the pipe. Profile changes need
control or a stopped Chrome.

**Viewer.** `/api/browser/viewer?instance_id=N` ([Web host](web-host.md))
relays the raw RFB stream of that instance's `display.sock`. Every 250 ms
the relay asks the service whether this device controls the display.
`RfbInputFilter` passes display messages always and key, pointer and
clipboard messages only from the controller, and on loss of control sends
releases for anything still held. A new `display` number (Chrome
restarted) ends the relay. One input message is at most 256 KiB.

**Page size.** `viewport` selects `laptop` (1440 × 900), `desktop`
(1920 × 1080), `tablet` (820 × 1180), `phone` (390 × 844) or the window's
own size, through `Emulation.setDeviceMetricsOverride` on every attached
tab; tablet and phone add touch. A larger size than the window is drawn
scaled down, with pointer input scaled to match, until the service exits.

**A tool call.** `BrowserTool` (`src/tools/browser.cc`) maps actions to
service operations. `status` and `release` need no lease; everything else
acquires first. An acting action (`open`, `click`, `type`, `press`,
`fill_saved`, `scroll`, `back`, `forward`, `reload`, `drag`, `batch`,
`upload`) runs, then the tool probes every 250 ms until two probes in a
row agree and the document is loaded, at most 8 s, then observes. The
result is what was done, a `SUSPECTED BLOCK` line if the page looks like a
bot check, rate limit or captcha, the page as `URL`, `Title`, numbered
`Elements` and up to 12,000 characters of text (`kPageTextChars`), and a
`View ID` with the viewport size. For the page the last result showed,
only changed lines are sent when that is shorter. `read` pages further or
returns lines matching `find`; `observe` adds a JPEG screenshot of exactly
the visible viewport at one pixel per CSS pixel; `console` and `network`
read the tab's messages and requests.

`batch` takes 1 to 20 steps of `click`, `type`, `press`, `scroll` and
`drag` on one look, settles after each, stops at the first failure or
dialog, and returns the page once. An approval is remembered for an action
on a site (`origin`); the service refuses the action if the page has since
moved to another origin, or, off the web, to another look.

**Dialogs and files.** An open alert, confirm or prompt replaces the page in
every result until `press` Enter or Escape answers it; text typed before
Enter answers a prompt. Downloads go to `<cwd>/downloads` and are reported
with whichever result follows their end. `upload` takes only a file inside
the conversation's folder. `open` with `path` loads an `.html` or `.svg`
file of the folder through its preview address (`UAGENT_INTERNAL_PREVIEW`)
and appends the page's own problem report; any other file is sent inline as
markup, at most 4 MiB.

**Capture.** `capture` and `problems` draw a preview page
(`http://127.0.0.1:<port>/preview/…` only) in a throwaway browser context
with no profile, wait up to 10 s for fonts and `window.uagentReady`, and
return a PNG or a PDF ([Artifacts](artifacts.md)).

## Invariants and limits

- The service drives one operation per instance at a time; takeover cannot
  race an agent's input.
- The model never reads a saved password: `type` refuses password fields,
  `fill_saved` lets Chrome fill and reads back only the field's length, and
  page text shows password, one-time-code and card fields as `(filled)` or
  `(empty)`. A screenshot is taken only after a script that draws password
  fields as dots has run.
- `deploy/chrome-policy.json` blocks `chrome://password-manager`,
  `chrome://settings/passwords` and `/autofill`, `passwords.google.com`,
  `view-source:` and `devtools://`; opens PDFs externally (as downloads);
  and hides the warning about the automation flag.
- `browser::Origin` accepts only `http` and `https` with a plain host.

## Failure modes

- Without Chrome the first action fails with `Google Chrome is not
  installed`; in the container `chrome/status` turns that into
  `still downloading` or `could not be downloaded`.
- A Chrome that exits at startup reports a sandbox hint; a locked profile
  fails with `Chrome profile is in use`.
- A handover whose conversation's runtime is gone is cleared at the next
  maintenance pass.

- `deploy/chrome-update.sh` checks Google's `Release` with `gpgv` against
  the image's key, then `Packages` and the `.deb` by SHA-256, extracts to
  `chrome/<version>` and switches `current` by rename; the running
  version's directory is kept. It runs at start and daily (10 min after a
  failure, keeping what is installed); `UAGENT_CHROME=off` disables it.

## Source and tests

| Path | What |
| --- | --- |
| `include/browser/browser.h`, `src/browser/browser.cc` | service start, framing, timeouts, `Origin` |
| `include/browser/broker.h`, `src/browser/broker.cc` | instances, queue, leases |
| `include/browser/runtime.h`, `src/browser/runtime.cc` | Chrome and Xvnc, CDP, idle stop |
| `src/browser/runtime_control.cc` | takeover, handback, profiles, page size |
| `src/browser/runtime_page.cc`, `src/browser/runtime_act.cc` | looks, masking, actions, capture |
| `src/tools/browser.cc` | the `browser` tool |
| `src/web/browser_viewer.cc`, `src/web/rfb_filter.cc` | viewer relay and input filter |
| `deploy/chrome-update.sh`, `deploy/start.sh`, `deploy/chrome-policy.json` | container Chrome |
| `tests/unit/browser_test.cc`, `tests/fixtures/browser_runtime.py` | broker, tool, filter; stand-in Chrome and Xvnc |
| `web/tests/browser.spec.js`, `web/tests/browser-agent.spec.js` | viewer and handover in the client |
