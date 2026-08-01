# Testing

The default suite is hermetic and needs no API key:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`tests/unit/` covers local policy and protocols. `tests/integration.py` drives
the binary through HTTP and PTY fixtures. The context simulation and evaluator
helper test are also provider-free. Fuzz targets run in CI.

## Live route scenarios

`uagent eval` runs billable, cost-capped workflows in a fresh workspace and
home for every model and trial. It writes a report and updates
`~/.uagent/config/routes.json`.

```sh
uagent eval \
  --model deepseek/deepseek-v4-flash \
  --model nvidia/nemotron-3-super-120b-a12b:free \
  --scenario checkpoint --repetitions 3 \
  --max-reported-cost 0.10 --report /tmp/uagent-route-ab.json
```

Use `--artifacts PATH` to retain workspaces and traces. `--scenario all` runs
the default set; `prompt` and `checkpoint500k` are opt-in. Run `uagent eval
--list-scenarios` for the authoritative list. Cost is enforced between calls,
so one in-flight request may cross the allowance.

## Add a scenario

Keep one capability, prompt contract, and deterministic verifier together:

1. Add `evaluate_<name>(binary, workspace, home, env) -> Result` to
   `tests/agent_workflow_live.py`. Use `run_agent` for isolation, events,
   timeout, and resource accounting.
2. Verify events, output, and workspace snapshots. Put explicit failures in
   `Result.contract`; never accept the model merely claiming success.
3. Register it once in `SCENARIOS`. Set `default=False` for expensive,
   destructive, provider-specific, or stress scenarios.
4. Cover new parsing, metrics, report, or profile logic in
   `tests/live_evaluation_test.py` without a provider.
5. Run Ruff and CTest, then one low-cap live trial. Inspect the report and
   retained artifacts before increasing repetitions.

Never put secrets in prompts, fixtures, reports, or failures. Pin and checksum
downloads. Installed scenarios use only the Python standard library and the
µAgent binary.
