---
name: browser-use
description: Use Chrome through µAgent to inspect or interact with a user browser session or an isolated browser, including navigation, screenshots, forms, console, network, and page debugging.
requires-tools: chrome_session
---

# Use Chrome in µAgent

1. Call `chrome_session` before using browser tools. Choose `mode=user` only
   when the user wants their running Chrome; otherwise use `mode=isolated`.
   Start with `toolset=slim` for navigation, evaluation, and screenshots.
2. Treat session selection as successful only when its page health check
   passes. If the user session has no usable page, ask the user to open a normal
   tab in the shared Chrome window and retry. Switch to `toolset=full` when the
   slim bridge hides the upstream error or the task needs granular page,
   console, network, or performance tools.
3. Use the registered `chrome-devtools_*` tools. Tool names can change when the
   session toolset changes, so inspect the current catalogue after
   `chrome_session` rather than assuming a stale name.
4. After navigation or mutation, verify the page state or take a screenshot.
   Preserve the user's control over sensitive submissions and any explicitly
   user-owned browser step.

Do not assume Playwright, Puppeteer, Node modules, a browser websocket, or
Codex/Claude browser tools are installed. Do not recreate browser control with
`run` when the Chrome MCP is available.
