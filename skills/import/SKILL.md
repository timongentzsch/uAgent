---
description: Use only when the user invokes $import. Condense what Codex remembers about the user into µAgent global memories; skills and Claude Code memories are imported by `/skills import` and `/memory import`.
argument-hint: [codex]
requires-tools: read_path, grep, memory
---

# Import

Skills and Claude Code's memories need no model: tell the user to run
`/skills import` and `/memory import` (each lists what can be imported;
`all` imports it).

What does need you is Codex's memory, `~/.codex/memories/memory_summary.md`.
Read it. Where a line is worth confirming, `grep` `MEMORY.md` beside it for
that line; never read `MEMORY.md` whole, it is large.

Condense it into at most 32 memories that hold true in any folder: one
durable preference or lesson each, under 2 KiB, ending with a `Why:` line.
Cover every durable preference and tip, not only the first few. Skip anything
that only matters in one named repository or product (how one product's
screens should match another's, say), one task, or that a command could look
up.

Show the planned names with a line each and wait for a yes, unless the user
already said to go ahead. Then save each with `memory set global/NAME`. Say
how many you saved and which you left out.
