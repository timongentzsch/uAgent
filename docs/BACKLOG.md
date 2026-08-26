# Improvement backlog

Hypotheses argued from evidence but not yet implemented. The `self-improve`
skill reads this in step 3; it holds candidates, not commitments, and an entry
leaves the file the moment it is implemented or refuted. Published numbers here
motivate a candidate — they never substitute for a measurement taken in the
loop.

## Browser round amortisation and snapshot budget

Clauses b (tokens) and e (timing).

The page representation, not the driver, is where browser tokens go. UI
representations account for 80-99% of prompt tokens on Mind2Web and
AndroidControl (arXiv 2512.13438), and returning only interactive elements
rather than every node cuts an untruncated accessibility snapshot by 51-79%
(dev.to/kuroko1t, February 2026: Playwright MCP 14.5-19.4k tokens per step
against 3.0-7.8k for the same pages). Anthropic's Chrome extension answers the
round half of this with `browser_batch`, one call carrying several actions.

`skills/browser-use/SKILL.md` already spends bytes well — `find`, subtree and
`--depth` snapshots, `--raw`, screenshots only on demand. It says nothing about
rounds. Several `playwright-cli` commands chained in one `run` call share the
session daemon and work today, verified against a live page; the skill never
says so, and one `run` per click is the result. `playwright-cli` is an external
binary, so this is guidance to write, not a primitive to build.

The rounds are measured. Across 40 sessions since 2026-08-01, `playwright-cli`
is 10.0% of every command `run` executes — 1036 calls in six sessions — and it
arrives in streaks where each consecutive round carries a single call: streaks
of 2, 3, 6, 14, 23, 27, 38, 41 and 94. That is the cost; whether guidance
moves it is not measured, and a live A/B already refuted one batching overlay.

Still needed before writing any: a scenario scoring rounds rather than chaining
syntax, so the gate cannot reward the mechanism over the outcome, and
`eval.py --run` against a live route, because scripted scenarios measure the
harness rather than whether the wording persuades a model.

Risk: a chain reports one exit status, so a mid-chain failure is harder to
attribute than a failed single call. Weigh that against the rounds saved.
