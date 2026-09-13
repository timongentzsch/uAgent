---
name: fusion
description: Use only when the user invokes $fusion. Coordinate one retained sidekick for a scoped implementation while the current agent remains responsible for decisions, review, and the final answer.
argument-hint: TASK
requires-tools: subagent, read_path, grep, run
---

# Fusion

Optimize total resources for a verified result. Keep tiny tasks, unresolved
architecture, consequential definitions, evaluation criteria, and final review
with the lead. Use `UAGENT_SUBAGENT_MODEL` as configured; if it inherits the
lead route, disclose that and make no savings claim.

## Start or recover the sidekick

List collaborators first after compaction or when the identity is uncertain.
Reuse the one persistent collaborator. Otherwise spawn it with
`persistent=true`, `mode=full`, and `background=false`. Give it this directive:

> Execute only the scoped brief. Surface decisions that change the plan. Return
> concise changes, verification evidence, reusable runtime handles, and open
> questions. Do not commit, publish, change permissions, or delegate further.

Do not override the configured worker model unless the user chose a route.
Follow-ups retain the original model, mode, limits, and runtime. A missing live
runtime may restore conversation, but its processes and shell state are gone;
inspect the workspace and explicitly re-brief before acting.

## Handoff loop

1. Resolve interfaces, assumptions, and authority that affect the design.
2. Send a compact brief with the goal, decisions, constraints, relevant paths,
   success criteria, narrow checks, and conditions that return control. Relay
   relevant user requirements; do not copy the conversation.
3. Let the sidekick inspect, edit, test, and fix one useful phase. Use `message`
   only to guide an active handoff. Idle guidance waits for an explicit
   `followup`; it does not start work.
4. Review the actual diff, artifacts, and decisive execution evidence. Batch
   corrections into one follow-up. Do not repeat unchanged successful checks.
5. After two failed correction rounds without a new hypothesis, take over or
   report the blocker.
6. Stop the retained runtime when Fusion work is settled. The lead reports the
   changes, validation, limitations, and remaining work.

Use blocking handoffs so lead and sidekick never edit concurrently. Worker
completion is evidence, not acceptance. Preserve errors, truncation, partial
results, and unknown cost as unknown.

Ask the sidekick to finish with:

```text
Outcome: completed / blocked / partial
Changes: files and material behavior
Validation: commands and actual outcomes
Runtime: reusable activity handles and whether they remain live
Open questions: unresolved decisions or failures
```
