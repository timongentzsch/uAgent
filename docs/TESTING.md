# Testing

The default suite is hermetic and needs no API key:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`tests/unit/` covers local policy and protocols. One shared HTTP/PTY fixture
drives isolated `runtime`, `tools`, `ui`, `providers`, `checkpoint`, `mcp`, and
`delegation` CTest processes. The context simulation and evaluator helper test
are also provider-free. Fuzz targets run in CI.

## Live route scenarios

`uagent eval` runs billable, cost-capped workflows in a fresh workspace and
home for every model and trial. Reports are evidence; profiles change only
with explicit `--certify`.

```sh
uagent eval \
  --model deepseek/deepseek-v4-flash \
  --model nvidia/nemotron-3-super-120b-a12b:free \
  --scenario all --repetitions 2 \
  --no-memory \
  --max-reported-cost 0.10 --report /tmp/uagent-route-ab.json
```

Use `--artifacts PATH` to retain workspaces and traces. `--scenario all` runs
the default set; repeat `--scenario` to build a focused suite. `prompt` and
`checkpoint500k` are opt-in. Run `uagent eval --list-scenarios` for the
authoritative list. `memory` tests relevant get, explicit set, and noise
suppression; it must not be combined with `--no-memory`. Cost is enforced
between calls, so one in-flight request may cross the allowance.

Add `--certify` only when the selected suite represents your intended route
use. It requires multiple repetitions, records that exact suite, and updates a
profile only when every trial passes. Effort recommendations compare identical
scenario sample sets rather than treating one task difficulty as universal.

## Add a scenario

Keep one capability, prompt contract, and deterministic verifier together:

1. Add `evaluate_<name>(binary, workspace, home, env) -> Result` to
   `tests/agent_workflow_live.py`. Use `run_agent` for isolation, events,
   timeout, and resource accounting.
2. Verify events, output, and workspace snapshots. Put explicit failures in
   `Result.contract`; never accept the model merely claiming success.
3. Register it once in `SCENARIOS`. Set `default=False` for expensive,
   destructive, provider-specific, or stress scenarios.
4. Cover new behavior in `tests/live_evaluation_test.py`. Generic trace and
   contract logic belongs in `eval_runtime.py`; certification belongs in
   `eval_profiles.py`.
5. Run Ruff and CTest, then one low-cap live trial. Inspect the report and
   retained artifacts before increasing repetitions.

Never put secrets in prompts, fixtures, reports, or failures. Pin and checksum
downloads. Installed scenarios use only the Python standard library and the
µAgent binary.
