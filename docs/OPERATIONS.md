# Operations

µAgent is a local single-user POSIX CLI for macOS and Linux, not an OS sandbox
or multi-tenant service.

## Bounds

| Concern | Default |
| --- | ---: |
| request / complete turn | 300 / 900 s |
| model rounds / tool calls | 40 / 100 |
| subagent depth / rounds / calls | 2 / 25 / 60 |
| reported turn cost | $1 |
| request / response | 64 / 32 MiB |
| source read / grep | 32 KiB / 200 matches |
| tool result / batch | 8,000 / 16,000 characters |
| background jobs / safe workers | 8 / 4 |
| attachment / terminal image | 10 / 10 MiB |
| checkpoint hint / urgent / emergency | 65 / 85 / 95% |
| removed-trace archive | 16 MiB |

Files, edits, instructions, memories, skills, schemas, MCP traffic, shell logs,
and Python scratch scripts have additional fixed bounds in `RuntimeConfig` and
their owning modules. Raise a bound only with a representative measurement.

Child processes are credential-sanitized. Approved shell commands alone may
receive explicitly named variables through `UAGENT_SHELL_ENV_ALLOW`. Project
`.uagent/.config` and `.mcp.json` require trust.

Dollar limits are enforced between calls and require provider-reported
`usage.cost`; output marks missing cost as unavailable. One in-flight request
may cross the remaining allowance. Budgeted delegation runs one child at a time
with the remaining allowance.

## Release

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
UAGENT_BUILD_DIR=/tmp/uagent-build UAGENT_PREFIX=/tmp/uagent-prefix ./install.sh
/tmp/uagent-prefix/bin/uagent --version
```

CI adds warnings-as-errors, sanitizers, TSan, parser fuzzing, coverage, Google
C++ style, and native Linux/macOS builds. Before a tag, verify the installed
archive, one real turn per supported route, Chrome isolated and user attach,
one private debug trace, and relevant [`uagent eval`](TESTING.md) scenarios.

The composite Action verifies a release SHA-256 and runs `--yolo --json` with
a reported-cost budget. It requires provider credentials in the job environment
and a trusted checkout. Never expose secrets while running untrusted fork code;
pin both the Action ref and release version.

## Failure triage

- Headless failures use nonzero exit status and a complete JSON envelope.
- MCP logs live under `~/.uagent/mcp`; debug traces are opt-in and sensitive.
- Corrupt sessions are reported and left untouched.
- Route-profile mismatches self-heal and should trigger recertification.
- Managed processes are reaped on catchable exits; `SIGKILL` cannot guarantee
  cleanup.

Local state and removal paths are listed in [PERSISTENCE.md](PERSISTENCE.md).
