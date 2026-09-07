---
name: self-improve
description: Run one bounded source-improvement attempt on µAgent, or operate its measured incumbent/successor comparison, promotion and rollback workflow.
argument-hint: [attempt|status|start|verdict GENERATION|rollback]
requires-tools: run, read_path, edit_file, grep
---

# Self-improve

For a source-improvement attempt, follow `${SKILL_DIR}/INSTRUCTION.md`. The
controller supplies the source, fixed instruction, route, limits and existing
gates. Stop after one justified candidate or no validated improvement.

Record `.uagent-improvement.json` before the substantive edit:

```json
{
  "schema": "uagent.improvement.claim.v1",
  "hypothesis": "one falsifiable sentence",
  "measurement": "what observable behavior will improve",
  "verify_command": "the command that demonstrates the improvement"
}
```

The controller requires the check to exit 0 on the candidate and 1 on the
original source. New focused measurement files can be added, but existing
verifier files and protected inputs must stay byte-identical. A passing check
alone does not establish a useful improvement: explain why its result matters.

For an operator request, run `python3 ${SKILL_DIR}/scripts/experiment.py --help`
and the relevant subcommand's `--help`. The sequence is `init`, `discover`,
`gate`, `replay`, `continue`, `verdict`. Use the pinned controller path returned
by `init`, an explicitly selected executor/skill bundle, frozen route config,
and reviewed route authority and limits. Never use normal subagent delegation
to choose the executor version.

Promotion requires a `promote` verdict and explicit authorization:
`promote --approve`. `rollback` restores the prior bundle/source pointer.
Installing a promoted bundle into the user's normal µAgent is a separate action.
Do not alter budgets, state, measurements or the controller to rescue a failed
attempt. A stopped or inconclusive experiment is a valid result.
