---
name: self-improve
description: Self-improve µAgent: run one measured iteration — scan recent sessions for waste, propose Pareto-optimal changes (hardware, tokens, readability, capability, timing, generality), verify against committed baselines, scan for slop.
argument-hint: [focus, e.g. "token", "capability", or "another iteration"]
---

# One improvement iteration

Improve the harness against evidence, not impressions. Every claim in the final
report is a number produced by a command in this file, or it is not made.

## Constitution

A change must improve at least one clause and regress none. When two clauses
conflict, say so, measure both sides, and let the human decide.

| | Clause | Measured by |
| --- | --- | --- |
| a | **Hardware cost** — memory, binary size, CPU | `audit.py` hardware row |
| b | **Token cost** — bytes charged to every request | `audit.py` token row, `eval.py` chars |
| c | **Readability and modularity** — coupling, file size, slop | `audit.py` readability row, rebuild fanout |
| d | **Capability** — what the agent can do, and how reliably | `eval.py` scores, new scenarios |
| e | **Timing** — wall clock for a turn and for the dev loop | `eval.py` wall, rebuild time, `ctest` time |
| f | **Generality** — no overfitting to the suite | `audit.py` representativeness row |
| g | **No slop** — no bloat in code, comments or docs, new or old | `slopscan.py`, plus the diff pass in step 6 |

Ruling a clause out is a result. If a measurement shows no headroom, report the
number and propose nothing there.

## The loop

### 1. Measure reality first

```sh
uv run --frozen python benchmarks/session_metrics.py --since 2026-08-01
```

Real sessions say which paths are hot, which tools fail, which fill the
context, and which are slow. Improvements to a path nobody uses are noise.
Treat retired tool names in the output as renames, not as regressions.

### 2. Measure the build

```sh
uv run --frozen python benchmarks/audit.py build/debug/uagent
uv run --frozen python benchmarks/eval.py build/debug/uagent --check
```

The audit prints all six clauses and flags baseline regressions. The eval
scores end-to-end behaviour against committed scenarios.

### 3. Propose

Three to five changes, each naming its clause, its expected effect, its risk,
and the measurement that will confirm or refute it. Prefer the smallest diff
that reuses existing infrastructure. Present the plan and wait for approval.

Read **Carried items** below first: a hypothesis already argued once, with its
measurement named, is a cheaper candidate than a fresh one.

### 4. Implement

One concern per commit. Keep unrelated working-tree changes out of the index;
if a file mixes your change with someone else's, stage only your hunks.

### 5. Verify

```sh
cmake --build --preset debug -j 12 && ctest --preset debug -j 4
uv run --frozen python tests/integration.py build/debug/uagent --test NAME   # one case
./build/debug/uagent --emit-reference skills/uagent-config/references        # then diff
```

A behavioural change needs a scenario that fails without it. Prove the gate is
not vacuous: break the thing on purpose, watch the score drop, restore it.

### 6. Scan for slop

Not optional. Two passes, because they find different things: your own diff is
where new bloat is, and the tree is where *your centralising left bloat behind*
— the old body under a return that can no longer be reached, the caller that
kept its copy, the doc still naming the old file. None of that appears in the
diff that introduces the next change.

```sh
uv run --frozen python benchmarks/slopscan.py --verbose   # the whole tree
uv run --frozen ruff check --select ARG,ERA,F401,F841 tests benchmarks
git diff | grep -E '^\+\s*(//|#)'          # every comment you added
```

`slopscan.py` is heuristic and biased toward silence, so read what it reports
rather than trusting the count: a name with no body can be a deliberate
link-time trap, and two similar blocks are only duplication when they are the
same policy. Its counts are baselined, so the tree can only get cleaner.

Reject: comments that restate the code instead of explaining why; unused
parameters, symbols and imports; filler and AI-tell phrasing; stale references
to renamed or deleted files; the same sentence repeated across code, CHANGELOG
and docs; generated prose longer than the thing it documents; abstractions that
do not remove real duplication. Report what you found, including what you chose
not to fix.

### 7. Report and decide

Lead with the numbers, then the trade-offs, then a merge verdict. Name every
known gap. The human decides the merge.

### 8. Install, then say so

A merged round changes nothing until the binary is replaced, and the running
session is still the old build — nothing it reports about itself is true of the
new one. After the merge is agreed:

```sh
cmake --build --preset release -j 12 && ./install.sh
```

Installing is automatable and belongs in this step. Restarting is not: ending
the session the human is talking to would be a decision taken on their behalf.
Say plainly that a restart is needed, and let them choose the moment. The next
turn of any still-running session prints the same reminder on its own, because
`uagent` notices when the file it was launched from has been replaced.

## Invariants

Breaking one of these is a bug, not a trade-off.

- Host authority stays with the host: prompts, overlays and directives change
  text only, never permissions, tools or limits.
- The base prompt is byte-stable across refactors; `prompt_digest` in
  `skills/uagent-config/references/manifest.json` proves it.
- Generated references match the binary. CI diffs them.
- Every integration case is in `TEST_ORDER`; the suite refuses to run otherwise.
- Evals are hermetic and key-free by default. Live runs need `--run` and a
  cost cap.
- Baselines are updated deliberately, in a reviewed commit, never to make a
  red run green.

## Anti-overfitting

The suite is a proxy, and a proxy optimised hard enough stops measuring the
thing. Guard it:

- Keep scenario tool mix within reach of the real mix from step 1. `audit.py`
  fails when a tool above 5% of real calls has no scenario.
- Scripted scenarios measure the harness, not model quality. For prompt
  wording, use `--prompt-overlay` with `eval.py --run` against a live route.
- New scenarios start in the `capability` tier, which reports but does not
  gate. When one holds green, graduate it to `regression`.
- Prefer a scenario drawn from a real failure in step 1 over an invented one.

## Carried items

Candidates argued from evidence but not yet implemented. Take one in step 3,
or refute it there. Delete the entry either way — a list that only grows stops
being read.

### Browser round amortisation and snapshot budget (clauses b, e)

Published measurements say the page representation, not the driver, is the
browser token bill: UI representations are 80-99% of prompt tokens on Mind2Web
and AndroidControl (arXiv 2512.13438), and returning only interactive elements
rather than every node cuts an untruncated accessibility snapshot by 51-79%
(dev.to/kuroko1t, Feb 2026, Playwright MCP 14.5-19.4k tokens per step against
3.0-7.8k for the same pages). Anthropic's Chrome extension answers the round
half with `browser_batch`, one call carrying several actions.

`skills/browser-use/SKILL.md` already spends tokens well — `find`, subtree and
`--depth` snapshots, `--raw`, screenshots only on demand. It says nothing about
rounds. Several `playwright-cli` commands chained in one `run` call share the
session daemon and work today, verified against a live page; the skill simply
never tells the model so, and one `run` per click is the result.

Measure before writing anything: `session_metrics.py` for how many real turns
spend consecutive rounds on `playwright-cli`, and a scenario scoring rounds
rather than the chaining syntax, so the gate cannot reward the mechanism over
the outcome. A live A/B already refuted one batching overlay this way.

Risk: chained commands report one exit status, so a failure mid-chain is
harder to attribute than a failed single call. Weigh that against the rounds
saved rather than assuming it.
