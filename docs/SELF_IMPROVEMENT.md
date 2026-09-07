# Self-improvement

`self-improve` runs one bounded source-improvement attempt, then compares the
incumbent and successor executors on the same source. It replaces the former
prompt-overlay experiment. General-purpose eval overlays remain available in
`benchmarks/eval.py`.

There are two independent identities:

- **Executor A:** executable plus bundled skills, hashed and kept outside the
  writable trial tree.
- **Subject S:** a content/mode-hashed source snapshot. Git records its lineage;
  every trial receives a fresh extracted copy, never the user's checkout.

The fixed instruction is [INSTRUCTION.md](../skills/self-improve/INSTRUCTION.md).
The agent chooses one hypothesis, records its falsification check before the
substantive edit, implements one change, and stops. No hand-written task suite
is required. A no-op is a valid outcome, but cannot win.

## One generation

```text
A0 + S0 -> discovery -> verify source S1 -> build bundle A1
A0 + fresh S0  vs  A1 + fresh S0             replay
A0 + fresh S1  vs  A1 + fresh S1             continuation
verdict -> explicit promotion of (A1,S1), or keep (A0,S0)
```

Both sides get the same instruction, route/effort, frozen configuration,
allowed prior history and per-run limits. The successor never inherits the
discovery conversation. Continuation proposals remain artifacts.

The controller builds each proposal and runs the declared existing gates.
Pre-existing tests, benchmark files, build definitions and protected controller
inputs must remain byte-identical; new focused measurements may be added.
It runs the claimed check on both trees: candidate exit 0, original exit 1.
Only added measurement files are copied to the original. A missing command,
failed original build, always-passing check or source-mutating verifier fails.
This reproduces the claim; human review still decides whether the hypothesis
is worthwhile and whether its measurement rewards useful behavior.

Selection keeps outcomes, provider cost, tokens, time and tool failures separate.
A candidate must be eligible, lose no validated outcomes, stay within every
resource tolerance, and gain in at least one dimension. Equal outcomes can win
with lower resource use. Different measurement identities are incomparable.
Both replay and continuation must favor promotion; rejection, inconclusive
results, incomparable results, crashes and missing telemetry keep the incumbent.

## Running it

Use an explicit build, route configuration and authority declaration. A build
command must configure its fresh directory as well as compile it, for example a
small existing build script. Commands are argv strings, not implicit shells.

```sh
python3 skills/self-improve/scripts/experiment.py --root /private/experiment init \
  --source /path/to/source --binary /path/to/uagent --skills /path/to/skills \
  --sandbox-binary /path/to/trusted/uagent --config /path/to/route.config \
  --route provider/model --cost-authority /path/to/authority.json \
  --max-cost 1 --max-runs 5 --max-model-calls 8 --max-tool-calls 32 \
  --max-tokens 100000 --max-wall-seconds 300 \
  --artifact build/debug/uagent --build-command 'sh build-and-configure.sh' \
  --gate 'ctest --test-dir build/debug --output-on-failure'
```

`init` returns the pinned controller path. Use it for subsequent commands:
`discover`, `gate`, `replay`, `continue`, `verdict`, then optionally
`promote --approve`. `status` reports state; `rollback` restores the exact prior
pointer. Promotion does not install into the normal user prefix.

Authority uses the shared `uagent.eval.cost-authority.v1` format documented in
[testing](TESTING.md). Billable routes must report cost and enforce the declared
USD budget. Subscription routes require an explicit non-billable/cheap
declaration and all five bounded limits. Model names alone are not authority.
Each billable run reserves the same `max_cost / max_runs` allowance.
`max_runs` bounds aggregate calls, tokens and time by the product of the
per-run ceilings; the total USD ceiling is separate. Gates also have bounded
wall time and output. Exhausted or failed runs cannot be retried for free.

## Isolation and records

The controller invokes the pinned trusted binary's OS sandbox trampoline around
both the executor and build/verifier commands. Only the trial source, its fresh HOME and device files under `/dev` are
writable (device access permits headless output and shell redirection). The sandbox must work or the run fails; candidate
settings cannot disable this outer boundary. Linux uses Landlock; macOS uses
Seatbelt. This protects writes, not confidentiality: reads and network access
remain available for normal development. Only explicitly supplied configuration
is copied into the fresh HOME; ambient API credentials are not inherited.

The parent owns process-group cleanup, wall/output limits and trace monitoring.
Call/token overrun stops the process and invalidates the result; observation can
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
