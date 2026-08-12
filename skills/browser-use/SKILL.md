---
name: browser-use
description: Use token-efficient Playwright CLI browser automation for navigation, forms, screenshots, debugging, or repeatable web workflows.
requires-tools: run
---

# Browser automation

Use `playwright-cli` through `run`; do not add a browser MCP server. µAgent sets
a unique `PLAYWRIGHT_CLI_SESSION`, so commands reuse one browser daemon without
sharing state with another agent process.

```sh
playwright-cli open https://example.com
playwright-cli snapshot --depth=4
playwright-cli find "Sign in"
playwright-cli click e14
playwright-cli fill e22 "hello" --submit
```

- Prefer `find`, shallow snapshots, element snapshots, and `--raw` extraction;
  use screenshots only when visual evidence matters. Refs such as `e14` come
  from the latest snapshot.
- Use `attach --cdp=chrome` (or `--cdp=URL`) when the user requests their
  running Chrome, then navigate with `goto`; `open` would replace it with a
  managed browser. `detach` leaves Chrome running. Otherwise, `open` starts an
  isolated session and `close` ends it.
- Keep the daemon alive across related actions. Verify only the relevant state
  after mutations instead of taking a full snapshot after every command.
- For a known or recurring workflow, explore once, obtain resilient locators
  with `generate-locator REF --raw`, then write and run deterministic Playwright
  code rather than continuing an LLM click loop.
- Preserve user control over logins, sensitive submissions, purchases, and any
  step the user explicitly owns.

If `playwright-cli` is unavailable, report the one-time official install:
`npm install -g @playwright/cli@latest`.
