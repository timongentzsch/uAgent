# Self-improvement

`self-improve` targets one architectural harness improvement and prepares a human
review package. Optional trials compare incumbent and successor executors on
the same source. It replaces the former
prompt-overlay experiment. General-purpose eval overlays remain available in
`benchmarks/eval.py`.

There are two independent identities:

- **Executor A:** executable plus bundled skills, hashed and kept outside the
  writable trial tree.
- **Subject S:** a content/mode-hashed source snapshot. Git records its lineage;
  every trial receives a fresh extracted copy, never the user's checkout.

The fixed instruction is [INSTRUCTION.md](../skills/self-improve/INSTRUCTION.md).
The agent is instructed to record one hypothesis and falsification check before
the substantive edit, implement one change, and stop. The controller reads the
claim after execution, so preregistration is not mechanically enforced.

The target is a shared architectural mechanism with effects across distinct
development workflows: context construction, tool execution, state ownership,
recovery or duplicated runtime paths, for example. The agent compares promising
options by expected impact, evidence, complexity and verification cost, then
implements one coherent change with before/after evidence per workflow.
Structural simplification can be demonstrated through removed duplication and
preserved consumer behavior; it does not imply a measured performance gain.
Patch size is not a selection criterion. An isolated edge-case fix remains an
incidental finding even when it lives in a widely used function. If the frozen
budget cannot support a justified architectural change, the agent reports the
proposal and missing evidence instead of substituting an easy micro fix.
Broader performance claims still need representative held-out tasks. A no-op
is a valid outcome, but cannot win.

## One generation

```text
preflight S0 -> A0 + S0 -> discovery -> verify S1 -> build A1 -> human review

Optional exploratory comparison:
A0 + fresh S0  vs  A1 + fresh S0             replay
A0 + fresh S1  vs  A1 + fresh S1             continuation
verdict -> explicit promotion of (A1,S1), or keep (A0,S0)
```

Both sides get the same instruction, route/effort, frozen configuration,
allowed prior history and per-run limits. The successor never inherits the
discovery conversation. Continuation proposals remain artifacts.

The controller snapshots each proposal into a fresh temporary source tree and
HOME before building it and running the declared existing gates. Discovery's
build artifacts and ancestor skills cannot leak into verification.
Pre-existing tests, benchmark files, build definitions and protected controller
inputs must remain byte-identical; new focused measurements may be added.
It runs the claimed check on both trees: candidate exit 0, original exit 1.
Only added measurement files are copied to the original. A missing command,
failed original build, always-passing check or source-mutating verifier fails.
This reproduces the claim; human review still decides whether the hypothesis
is worthwhile and whether its measurement rewards useful behavior.
`gate` and `status` report `source_validated: true` once the source check succeeds
and the successor bundle is built. That result is retained even if later A/B
comparisons are inconclusive: a validated source fix and a demonstrated gain
in recursive improvement are separate results.
Architectural scope is an instruction and human-review criterion, not an
automatic controller classification. `source_validated` alone does not satisfy
that criterion; the reviewer checks the mechanism and per-workflow evidence.

Before discovery, the unchanged snapshot must pass the same build and existing
gates in a fresh source tree and HOME. Failure records `preflight_failed`, uses
no model calls, and produces no candidate verdict. This detects an unusable
baseline; it does not automatically diagnose whether code or infrastructure
caused the failure.

`review` creates an applicable `proposal.patch`, a readable `review.md`, and
`review.json` containing the agent's change summary, measured-impact narrative,
generality assessment, limitations, recommendation and proposed commit/PR title.
Controller evidence is separate: preflight, before/after check results, gate
logs, discovery measurements and optional recursive verdict. Its review ID
binds the source identities, exact patch, assessment and recorded evidence.
Missing assessment blocks review, not source validation.

The operator presents this material and independently reviews the measurement
and relevant edge cases before asking the human to authorize a specific commit
or PR. A report or passing check does not authorize applying, committing,
publishing or activating the candidate. There is no controller commit/PR
endpoint: presentation and authorization are operator responsibilities. An
existing authorization for the exact reviewed action need not be requested
again. Revisions require revalidation and an updated review.

Selection keeps outcomes, provider cost, tokens, time and tool failures separate.
A candidate must be eligible, lose no validated outcomes, stay within every
resource tolerance, and gain in at least one dimension. Equal outcomes can win
with lower resource use. Different measurement identities are incomparable.
Both replay and continuation must favor promotion; rejection, inconclusive
results, incomparable results, crashes and missing telemetry keep the incumbent.
Pair order alternates across trials. These comparisons are exploratory: the
default one pair and 10% thresholds provide no statistical confidence, and
verdicts explicitly report that generalization is not established. For a
performance claim, use `benchmarks/eval.py` with repeated trials and separately
held-out, independently reviewed tasks shared by both versions. Existing
repository scenarios are visible during discovery and are not held out.
The runner reads `benchmarks/scenarios/`; keep held-out scenarios in a separate
evaluation checkout that is unavailable during discovery, and evaluate the
frozen binaries there. No held-out task bank is supplied by this change.
The [research basis](SELF_IMPROVEMENT_RESEARCH.md) maps these choices to sources
and distinguishes published findings from our implementation decisions.

## Running it

Use an explicit build, route configuration and authority declaration. A build
command must configure its fresh directory as well as compile it, for example a
small existing build script. Commands are argv strings, not implicit shells.

```sh
python3 skills/self-improve/scripts/experiment.py --root /private/experiment init \
  --source /path/to/source --binary /path/to/uagent --skills /path/to/skills \
  --sandbox-binary /path/to/trusted/uagent --config /path/to/route.config \
  --route provider/model --cost-authority /path/to/authority.json \
  --max-cost 1 --max-runs 5 --max-tool-calls 32 \
  --max-tokens 100000 --max-wall-seconds 300 \
  --artifact build/debug/uagent --build-command 'sh build-and-configure.sh' \
  --gate 'ctest --test-dir build/debug --output-on-failure'
```

`init` returns the pinned controller path. Use it for subsequent commands:
`discover`, `gate`, `review`, then present the report and patch to the human.
An architectural source change can reach human review without recursive trials;
the reviewer still assesses its scope and evidence across affected workflows.
For an exploratory executor comparison, run `replay`, `continue`, `verdict`,
then regenerate `review`. After explicit human approval of that review,
`promote --approve --review-id ID` requires a promoting verdict and a current
review ID. `status` reports state; `rollback` restores the exact prior
pointer. Promotion does not install into the normal user prefix.

For a trusted local source run whose tests exercise native sandboxing, select
`--gate-mode host` at `init` (as needed for µAgent's full macOS CTest suite).
This runs build, test and measurement commands with ordinary host permissions
in fresh temporary trees, while the discovery and comparison executors remain
sandboxed. It permits tests to create their own sandboxes and inspect processes.
Host mode executes candidate code with your user permissions; a temporary tree
is not a security boundary. Use the default `--gate-mode sandbox` when that
authority is inappropriate. The mode is frozen before discovery and recorded
with every gate result; a failed sandbox gate never triggers a host retry.
Both modes require every existing gate and candidate-pass/original-fail check.
For unattended candidate execution, use disposable infrastructure with an
appropriate isolation boundary and controlled resources; this controller does
not provision a VM or make host execution safe for untrusted code.

Authority uses the shared `uagent.eval.cost-authority.v1` format documented in
[testing](TESTING.md). Billable routes must report cost and enforce the declared
USD budget. Subscription routes require an explicit non-billable/cheap
declaration and all five limit fields. For self-improvement, set the authority's
`limits.max_model_calls` to `0` to allow unlimited model calls. Other authority
limits remain mandatory and bounded; general-purpose live eval still requires
a positive model-call limit. Model names alone are not authority.
The controller defaults to no model-call cap (`--max-model-calls 0`); any
positive cap in either the controller options or route authority still applies.
Wall time, total tokens, tool calls and run count continue to bound each generation.
Each billable run reserves the same `max_cost / max_runs` allowance.
`max_runs` bounds aggregate tokens and time by the product of the
per-run ceilings; the total USD ceiling is separate. Gates also have bounded
wall time and output. Exhausted or failed runs cannot be retried for free.

## Isolation and records

The controller always invokes the pinned trusted binary's OS sandbox trampoline
around executors, and around verification commands in the default sandbox mode.
Inside that boundary, only the trial source, its fresh HOME and device files under `/dev` are
writable (device access permits headless output and shell redirection). The sandbox must work or the run fails; candidate
settings cannot disable this outer boundary. Linux uses Landlock; macOS uses
Seatbelt. This protects writes, not confidentiality: reads and network access
remain available for normal development. Only explicitly supplied configuration
is copied into the fresh HOME; ambient API credentials are not inherited.

The parent owns process-group cleanup, wall/output limits and trace monitoring.
Configured call/token overrun stops the process and invalidates the result; observation can
follow an in-flight call, so it is not a provider-side token reservation.
Reported-cost authority must supply the hard financial boundary independently.
The trace is instrumentation evidence, not protection against an adversarial
binary falsifying telemetry. Protected instrumentation and independent checks
catch ordinary candidate regressions; this is not an adversarial optimizer.

State contains manifests, hash-addressed snapshots/bundles, measured runs,
gate outcomes, verdicts and an atomic active/previous pointer. The controller
is copied outside writable trials and its revision is checked at each command.
Detailed traces/configuration stay private; aggregate records contain paths,
hashes, claims and measurements. Do not publish private experiment directories.
Legacy overlay state and user configuration are not automatically deleted.

`tests/self_improve_controller_test.py` drives the loop with scripted executors
inside the real native sandbox. Native integration tests separately exercise
provider, process, PTY, trust and persistence behavior. A real-model generation
is evidence about that route/run only, not a general capability score.
