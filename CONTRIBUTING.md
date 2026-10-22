# Contributing

Keep µAgent small, bounded and explicit. Prefer one shared policy or helper
over local exceptions, and preserve behavior before redesigning a boundary.
First-party C++ follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

## Verify

Build and run the hermetic suite; it needs no API key or network:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
```

While iterating, run one case instead; [Testing](docs/TESTING.md#common-cases)
shows how.

Before a commit, run what CI checks on every change:

```sh
uvx --from clang-format==22.1.8 clang-format --dry-run --Werror \
  include/*/*.h src/*.cc src/*/*.cc \
  tests/unit/*.h tests/unit/*.cc tests/fuzz/*.cc benchmarks/*.cc
uv run --frozen ruff check .github tests benchmarks skills
uv run --frozen ruff format --check .github tests benchmarks skills
git diff --check
```

After changing a flag, slash command, setting or tool, regenerate the
reference the `uagent-config` skill ships; CI fails when it is stale:

```sh
build/debug/uagent --emit-reference skills/uagent-config/references
```

Builds are warning-clean under `-Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion -Wshadow -Wold-style-cast`, and the presets treat warnings
as errors. Make a narrowing or signedness change explicit where it happens
rather than widening the receiving type.

For web changes, run from `web/`:

```sh
npm ci
npm run format:check
npm run lint
npm test
npm run build      # type-checks first
npm run notices
npm run size
```

`web/dist` is build output and not committed; CI builds it once and embeds it
in every native build. A build tree first configured without `web/dist` is
terminal-only: after the first `npm run build`, configure it again with
`-DUAGENT_WEB=ON`. Commit `web/THIRD_PARTY_NOTICES.md` when dependencies
change; CI fails when it differs from a fresh `npm run notices`.
`npm run test:browser` runs the Playwright suite against a native host; see
[Testing](docs/TESTING.md#web-tests).

A macOS build does not show what CI's Linux checks report: cpplint,
clang-tidy, the generated references and a GCC 13 build with warnings as
errors. `.github/ci-local.sh` runs them before a push, the compiler checks
inside the Ubuntu image CI uses. It needs uv and Docker. The first run builds
everything and later runs only what changed; clang-tidy reads the changed
sources, or every source when a header changed. Both scale with the cores
Docker is given.

## Changes

- Add tools through `MakeTool`. Set approval, mutation, concurrency, timeout,
  result and call limits, and return a typed `ToolResult`.
- Put generic HTTP and SSE behavior in transport, wire decoding in the route
  adapters, and provider quirks in provider normalization.
- Route filesystem access through the shared path policy. Reject unsupported
  file types before opening them.
- Give asynchronous work one owner, a deadline, bounded output, cancellation
  and a tested shutdown path. Never call external code while holding a mutex.
  Use stop tokens for supervised work and the shared wake descriptor for
  socket loops; destruction wakes and joins the owning thread.
- Own a file descriptor with `Fd` (`include/core/fd.h`), never a bare `int`
  closed on each early return. Mark the mutex that guards shared state with
  `UAGENT_GUARDED_BY`, and lock-holding helpers with `UAGENT_REQUIRES`
  (`include/core/thread_annotations.h`).
- Declare settings in `include/core/config_registry.h` and read
  session-static values from `RuntimeConfig`; keep environment accessors for
  deliberately dynamic route and delegation state. Avoid unbounded inputs,
  queues and arithmetic.
- Tool children use the central `ChildEnvironment` credential policy. Session
  workers inherit the trusted application environment. Never log secrets.
- Keep persistence versioned, and validate complete temporary state before
  replacing live state.

Test as [Testing](docs/TESTING.md#guidelines) says. Parser changes must
preserve chunk-boundary equivalence.

## Code conventions

- The C++ build has no exceptions (`-fno-exceptions`). An operation that can
  fail returns `bool` and fills a `std::string& error`; "absent" is
  `std::optional`. At the tool boundary the result is a typed `ToolResult`
  (`include/tools/tool.h`). Only the wire boundary builds an `{"error": ...}`
  JSON object: the `*Control(const json&)` handlers, such as `SkillControl`
  and `ScheduleControl`, and the per-action functions they dispatch to.
- Keep headers and includes in the layer order
  [Architecture](docs/ARCHITECTURE.md#build-layers) describes;
  `tests/boundary_test.py` enforces it.
- Web features (`web/src/features/<name>/`) import from `shared/` and
  `state/`, not from each other. Every request to the host goes through
  `web/src/state/api.ts`.

## Client boundaries

The terminal and web clients submit commands and render runtime events.
Authority, accounting and conversation changes stay in the C++ runtime. In
the browser, `useHost` (`web/src/state/use-host.ts`) owns the subscription,
`apply-event.ts` applies each stream event with the reducers in `store.ts`,
and components own only presentation and local interaction state. Derive
status from the shared snapshot; never keep a second conversation or
execution state machine in a component. Use semantic controls, visible focus
and accessible status text.

## Release

Before tagging, besides the build and tests above, run the `release`,
`sanitize`, `tsan`, `fuzz` and `coverage` presets, and verify the installed
archive, one real turn per supported wire API, both Playwright browsers and
one debug trace. Benchmarks are trend signals, not gates.

### Releasing

1. In a pull request, set `project(uagent VERSION ...)` in `CMakeLists.txt`
   and turn the `## Unreleased` heading in `CHANGELOG.md` into
   `## <version>`.
2. Merge it and wait for CI on master to pass. It builds and uploads the
   release archives and the web bundle, which are kept for 7 days.
3. Tag that commit and push the tag:
   `git tag v<version> && git push origin v<version>`.

The `Release` workflow (`.github/workflows/release.yml`) does not rebuild. It
finds the passing master run for the tag's commit, downloads its `release-*`
and `web-dist` artifacts, checks that their names carry the tag's version,
signs `SHA256SUMS` with cosign, attests the files and publishes the GitHub
release. If the commit has no passing master run or the artifacts expired, it
first runs the full `ci.yml` for the tag. Merge, wait and tag within 7 days to
keep the fast path.
