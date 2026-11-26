---
name: fusion
description: Use only when the user invokes $fusion. Coordinate one sidekick agent for a scoped implementation while the current agent remains responsible for decisions, review, and the final answer.
argument-hint: TASK
requires-tools: agent_start, read_path, grep, run
---

# Fusion

Optimize total resources for a verified result. Keep tiny tasks, unresolved
architecture, consequential definitions, evaluation criteria, and final review
with the lead. The sidekick runs on the lead's model unless the user named one
for it; on the lead's model, disclose that and make no savings claim.

## Start or recover the sidekick

After compaction or when its id is uncertain, find it with `agents`
`action=list`. Start it only once, with `agent_start`, `edits=folder` and
`wait=true`, passing `model` only when the user chose one for the sidekick.
Open the first brief with:

> Execute only the scoped brief. Surface decisions that change the plan. Return
> concise changes, verification evidence, and open questions. Do not commit,
> publish, change permissions, or start agents.

Send every later handoff to its id with `agent_send`, which keeps its
conversation and model, then end your turn: its report starts your next one.
Its runtime may stop between handoffs, so processes and shell state from an
earlier one are gone.

## Handoff loop

1. Resolve interfaces, assumptions, and authority that affect the design.
2. Send a compact brief with the goal, decisions, constraints, relevant paths,
   success criteria, narrow checks, and conditions that return control. Relay
   relevant user requirements; do not copy the conversation.
3. Let the sidekick inspect, edit, test, and fix one useful phase. Use
   `agent_send` during a handoff only to guide it; a message to an idle
   sidekick starts a new run, so hold that guidance for the next brief.
4. Review the actual diff (`agents` `action=read`, `part=diff`), artifacts,
   and decisive execution evidence. Batch corrections into one handoff. Do not
   repeat unchanged successful checks.
5. After two failed correction rounds without a new hypothesis, take over or
   report the blocker.
6. The lead reports the changes, validation, limitations, and remaining work.

Never edit while the sidekick works. Its report is evidence, not acceptance.
Preserve errors, truncation, partial results, and unknown cost as unknown.

Ask the sidekick to finish with:

```text
Outcome: completed / blocked / partial
Changes: files and material behavior
Validation: commands and actual outcomes
Open questions: unresolved decisions or failures
```
