---
name: self-improve
description: Self-improve µAgent: run one measured iteration — scan recent sessions for waste, propose Pareto-optimal changes (hardware, tokens, readability, capability, timing, generality), verify against committed baselines, scan for slop.
argument-hint: [focus, e.g. "token", "capability", or "another iteration"]
---

# One improvement iteration

Improve the harness against evidence, not impressions. Every claim about an
effect is a number produced by a command in this file, or it is not made.
Whether that effect is worth having is the one judgement no command returns;
clause h is where that argument goes, and it is argued in the open.

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
| g | **No slop** — no bloat in code, comments or docs, new or old | `slopscan.py`, plus the diff pass in step 7 |
| h | **Design sense** — should this exist, at this size, in this place | step 4; argued, never a number |

Ruling a clause out is a result. If a measurement shows no headroom, report the
number and propose nothing there.

Clause h is not a tiebreaker applied once the numbers are in. A change can
improve every measurable clause and still be the wrong thing to build, and h
can reject it on that ground alone. No measurement overrules h; a measurement
is evidence about the world, and h is a claim about what we should do with it.

## The instruments are in scope

`eval.py`, `audit.py`, `slopscan.py`, the scenarios and their fixtures are part
of the harness, not neutral observers of it. A blind instrument hides the work
worth doing, so a round spent entirely on measurement is a legitimate round:
sharper tests give sharper insight, and insight is what the next change is made
of. Improving an instrument needs no separate justification — it competes for a
slot in step 3 like anything else.

Three rules stop that from turning into self-congratulation.

- **An instrument must be able to fail.** Prove it with fixtures or a planted
  defect before trusting a green result. A gate that cannot go red measures
  nothing and looks exactly like a clean tree.
- **Never change an instrument and the thing it scores in the same commit.**
  That is how a regression gets laundered into a baseline.
- **When a check cannot fire on this repository's real style, fix the check.**
  Narrowing the fixture until it passes encodes the blind spot as intent.

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

The audit prints its six rows and flags baseline regressions. The eval scores
end-to-end behaviour against committed scenarios.

### 3. Propose

Three to five changes, each naming its clause, its expected effect, its risk,
and the measurement that will confirm or refute it. Prefer the smallest diff
that reuses existing infrastructure. Present the plan and wait for approval.

A hypothesis argued in an earlier round and deferred is a legitimate candidate,
but it does not inherit its old justification: re-measure it in step 1 like any
other. `docs/BACKLOG.md` carries those, and an entry leaves it the moment it is
implemented or refuted.

### 4. Judge the design

Numbers say whether a change works. They never say whether it should exist, and
they are actively misleading about it: a saving of 96% is compelling until you
ask how often the thing runs. Answer these per surviving candidate, in writing,
before any code is written. An answer of "I don't know" is a stop, not a shrug.

- **Necessity.** What breaks if this never ships? Name the person or the run
  that hits it. If the honest answer is a number nobody can feel, drop the
  candidate and report the number as the result.
- **Size.** Does the abstraction have two real callers with the *same* policy?
  One caller is a wrapper wearing a helper's clothes; two callers that merely
  look alike are not duplication, and merging them invents a policy neither
  had.
- **Reversibility.** If this is wrong three rounds from now, what does undoing
  it cost? A one-way door — a schema, a baseline others fork from, a file
  everything imports — needs evidence proportional to the door.
- **Second order.** What does this make easier to do *badly*? What will be
  built next because this now exists, and do we want that thing?
- **Ossification.** A gate turns today's judgement into tomorrow's rule, and
  rules outlive their reasons. Name the policy it freezes and say plainly
  whether it is worth defending when it fires on someone else's work.
- **Deletion.** If it vanished in six months, who notices, and how? Something
  nobody would miss should not be built now.

The proposer is the worst reviewer of a proposal: by step 3 the case is already
argued and the reasoning is anchored. Get the questions answered by something
without that stake — the `review-agent` skill, or a `subagent` given the diff
or the plan and these questions and no argument in favour. When neither is
reachable, answer them yourself in writing and say that no independent review
happened, because an unreviewed judgement recorded as reviewed is worse than an
open one.

Rejections are the cheapest result this loop produces and the easiest to lose.
Carry them into step 8.

### 5. Implement

One concern per commit. Keep unrelated working-tree changes out of the index;
if a file mixes your change with someone else's, stage only your hunks.

### 6. Verify

```sh
cmake --build --preset debug -j 12 && ctest --preset debug -j 4
uv run --frozen python tests/integration.py build/debug/uagent --test NAME   # one case
./build/debug/uagent --emit-reference skills/uagent-config/references        # then diff
```

A behavioural change needs a scenario that fails without it. Prove the gate is
not vacuous: break the thing on purpose, watch the score drop, restore it.

### 7. Scan for slop

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

### 8. Report and decide

Lead with the numbers, then the trade-offs, then a merge verdict. Name every
known gap. The human decides the merge.

Report every candidate rejected in step 4 with the question that killed it, and
every candidate the numbers favoured that judgement changed the shape of. A
round that only reports what shipped hides its most transferable finding.

### 9. Install, then say so

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
- Published numbers motivate a hypothesis; they never stand in for a
  measurement taken here. Cite them to argue a candidate, not to justify a
  merge.
