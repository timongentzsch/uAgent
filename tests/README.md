# Tests

The suite is hermetic: no API key, no network. From the repository root:

```sh
cmake --preset debug && cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure            # everything
build/debug/uagent_tests -k Activity                # unit cases by substring
python3 tests/integration.py build/debug/uagent --group runtime --list
python3 tests/integration.py build/debug/uagent -k compaction
python3 tests/integration.py build/debug/uagent -j 8   # all groups, in parallel
```

`--test NAME` takes an exact name in both runners. The web tests, the
behavioral evaluation and what each CI job runs are in
[Testing](../docs/TESTING.md). The rest of this file maps the directory.

| Path | Contents |
| --- | --- |
| `unit/` | C++ unit tests, built into `uagent_tests` (CTest `core`) |
| `integration.py` | Runner for the Python integration suite; `--group` selects one CTest group |
| `integration_<group>.py`, `integration_runtime/` | Cases for the `runtime`, `tools`, `ui`, `providers`, `mcp`, `delegation`, `sandbox`, `web` and `management` groups |
| `integration_support.py`, `session_support.py`, `web_support.py`, `memory_fixture.py` | Shared mock-provider, session-protocol, web-host and memory helpers |
| `web_host.py` | Mock-backed native host for the browser tests in `web/tests/` |
| `boundary_test.py`, `wire_contract_test.py`, `ci_changes_test.py` | Source contracts (CTest label `source`) |
| `package_contents.py` | Validates a release archive and its bundled skill tree |
| `fuzz/` | libFuzzer targets for SSE framing and terminal input decoding, with corpora |
| `fixtures/` | Provider route contract, saved sessions, evaluation and browser-runtime fixtures |
