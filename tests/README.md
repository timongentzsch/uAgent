# Tests

Commands, scope and the behavioral evaluation are described in
[Testing](../docs/TESTING.md). This file maps the directory.

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
