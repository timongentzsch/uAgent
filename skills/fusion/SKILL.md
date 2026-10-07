---
name: fusion
description: Use only when the user invokes $fusion. Coordinate one durable sidekick child for a scoped implementation while the current agent remains responsible for decisions, review, and the final answer.
argument-hint: TASK
requires-tools: subagent, read_path, grep, run
---

# Fusion

Optimize total resources for a verified result. Keep tiny tasks, unresolved
architecture, consequential definitions, evaluation criteria, and final review
with the lead. The sidekick runs on the lead's route unless the user named one
for it; on the lead's route, disclose that and make no savings claim.

## Start or recover the sidekick

List agents first (`operation=list`) after compaction or when the identity
is uncertain. Reuse the one sidekick with `operation=followup`. Otherwise
spawn it with `name=sidekick`, `mode=full`, and `background=false`. Give it
this directive:

> Execute only the scoped brief. Surface decisions that change the plan. Return
> concise changes, verification evidence, reusable runtime handles, and open
> questions. Do not commit, publish, change permissions, or delegate further.

Pass `model` only when the user chose a route for the sidekick.
Follow-ups keep the conversation, model, mode and directive; each one is a
fresh process, so processes and shell state from an earlier handoff are gone.
Pass `background=false` on every follow-up.

## Handoff loop

1. Resolve interfaces, assumptions, and authority that affect the design.
2. Send a compact brief with the goal, decisions, constraints, relevant paths,
   success criteria, narrow checks, and conditions that return control. Relay
   relevant user requirements; do not copy the conversation.
3. Let the sidekick inspect, edit, test, and fix one useful phase. Use `message`
   only to guide a running handoff. A `message` to a sidekick that has
   finished starts a new run, exactly like `followup`, so hold guidance for an
   idle sidekick until the next brief.
4. Review the actual diff, artifacts, and decisive execution evidence. Batch
   corrections into one follow-up. Do not repeat unchanged successful checks.
5. After two failed correction rounds without a new hypothesis, take over or
   report the blocker.
6. The lead reports the changes, validation, limitations, and remaining work.

Use blocking handoffs so lead and sidekick never edit concurrently. Worker
completion is evidence, not acceptance. Preserve errors, truncation, partial
results, and unknown cost as unknown.

Ask the sidekick to finish with:

```text
Outcome: completed / blocked / partial
Changes: files and material behavior
Validation: commands and actual outcomes
Runtime: activity handles it started and whether they remain live
Open questions: unresolved decisions or failures
```
