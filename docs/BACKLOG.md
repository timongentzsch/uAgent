# Improvement backlog

Hypotheses argued from evidence but not yet implemented. The `self-improve`
skill reads this in step 3; it holds candidates, not commitments, and an entry
leaves the file the moment it is implemented or refuted. Published numbers here
motivate a candidate — they never substitute for a measurement taken in the
loop.

## Near-duplicate detection in slopscan

Clause g (slop).

`duplicate_block` hashes six normalised lines exactly, so a copy with one
renamed variable is invisible — and a renamed copy is precisely what a
half-finished extraction leaves. Normalising identifiers to positional
placeholders would catch those.

It was proposed and dropped in the round that added the fixtures, on the design
questions rather than on a measurement. Two blocks with the same shape and
different policy are not duplication, and consolidating them invents a policy
neither had; a standing findings list of "these look alike" applies steady
pressure toward exactly that merge. Its true-positive rate on this tree is
unknown, and unknown is the wrong basis for a new baseline.

What would settle it: implement the pass as a throwaway, run it once, and hand-
review every finding. If most are real extractions someone abandoned, it earns
code and a report-only baseline. If most are shape collisions, this entry is
refuted and should be deleted rather than carried.

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

Measure first: `session_metrics.py` for how many real turns spend consecutive
rounds on `playwright-cli`, then a scenario scoring rounds rather than chaining
syntax, so the gate cannot reward the mechanism over the outcome. A live A/B
already refuted one batching overlay on exactly that distinction.

Risk: a chain reports one exit status, so a mid-chain failure is harder to
attribute than a failed single call. Weigh that against the rounds saved.
