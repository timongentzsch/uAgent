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

The measurement prerequisite now exists: `browser_outcome_rounds` uses a
local deterministic page and scores outcome, model rounds, cumulative
context/snapshot characters, and recovery after an intermediate failure.
`eval.py` can repeat fresh trials and reports pass@1/pass@k/pass^k by
route/model/provenance cohort. The scripted fixture proves the grader, not that
new wording persuades a model.

No browser guidance changes until a live A/B wins on outcome-adjusted rounds.
The local routes report no cost, so the reported-cost authority mode stays
unavailable and `--max-cost` cannot bound this run. The eval's second mode can:
an operator declaration that names the exact route non-billable and cheap, with
all five limits, buys a bounded live run inside the global ceilings — and a
two-arm five-trial A/B is ten sessions, under the twelve-session cap. That
declaration is a human policy statement, never a model-name inference, so the
run waits on the operator rather than on a missing mechanism.

Risk: a chain reports one exit status, so a mid-chain failure is harder to
attribute than a failed single call. Weigh that against the rounds saved.
