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

While iterating, run one case instead of the suite
([Testing](docs/TESTING.md) has the rest):

```sh
build/debug/uagent_tests -k Activity
python3 tests/integration.py build/debug/uagent -k compaction
```

Before a commit, run what CI checks on every change:

```sh
uvx --from clang-format==22.1.8 clang-format --dry-run --Werror \
  include/*.h include/*/*.h src/*.cc src/*/*.cc \
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
-Wsign-conversion -Wshadow -Wold-style-cast`, and presets treat warnings as
errors. Make a narrowing or signedness change explicit where it happens rather
than widening the receiving type.

For web changes, run from `web/`:

```sh
npm ci
npm run format:check
npm test
npm run build
npm run notices
npm run size
```

`web/dist` is build output and not committed; CI builds it once and embeds
it in every native build. A build tree first configured without `web/dist`
is terminal-only; after the first `npm run build`, configure it again with
`-DUAGENT_WEB=ON`.
Commit `web/THIRD_PARTY_NOTICES.md` when dependencies change; CI fails when
it differs from a fresh `npm run notices`. `npm run test:browser` runs the
Playwright suite against a native host; see [Testing](docs/TESTING.md).

CI also runs cpplint and clang-tidy. Configure clang-tidy through the `tidy`
preset: it disables the precompiled header, which another clang cannot read.
On macOS use Homebrew LLVM with the Apple SDK, since upstream clang-tidy cannot
parse the SDK's libc++ headers without `-isysroot`:

```sh
uvx --from cpplint==2.0.2 cpplint --recursive --exclude=third_party \
  --exclude=tests/fixtures --extensions=h,cc \
  --filter=-build/c++17,-build/header_guard,-whitespace/indent_namespace,-readability/check \
  include src tests benchmarks

cmake --preset tidy
$(brew --prefix llvm)/bin/run-clang-tidy \
  -clang-tidy-binary $(brew --prefix llvm)/bin/clang-tidy \
  -p build/tidy -header-filter='.*/(include|src|tests|benchmarks)/.*' -quiet \
  -extra-arg=-isysroot$(xcrun --show-sdk-path)
```

Before a release, also run the `release`, `sanitize`, `tsan`, `fuzz` and
`coverage` presets. Benchmarks are trend signals, not correctness gates.

## Changes

- Add tools through `MakeTool`. Set approval, mutation, concurrency, timeout,
  result and call limits; return a typed `ToolResult`.
- Put generic HTTP/SSE behavior in transport, wire decoding in the route
  adapters, and provider quirks in provider normalization.
- Route filesystem access through the shared path policy. Reject unsupported
  file types before opening them.
- Give asynchronous work one owner, a deadline, bounded output, cancellation
  and a tested shutdown path. Do not call external code while holding a mutex.
  Use stop tokens for supervised work and the shared wake descriptor for
  socket loops; destruction wakes and joins the owning thread.
- Own a file descriptor with `Fd` (`include/core/fd.h`), never a bare `int`
  plus a `close` on each early return. Name the mutex that covers shared state
  with `UAGENT_GUARDED_BY`, and lock-holding helpers with `UAGENT_REQUIRES`
  (`include/core/thread_annotations.h`).
- Declare settings in `include/core/config_registry.h` and read session-static
  values from `RuntimeConfig`; reserve environment accessors for deliberately
  dynamic route and delegation state. Avoid unbounded inputs, queues and
  arithmetic.
- Tool children use the centralized `ChildEnvironment` credential policy.
  Session workers inherit the trusted application environment. Never log
  secrets.
- Keep persistence versioned and validate complete temporary state before
  replacing live state.

Add focused unit coverage for local behavior or one hermetic integration path
for externally visible behavior; do not cover one contract at several layers.
Parser changes must preserve chunk-boundary equivalence. See
[Testing](docs/TESTING.md).

## Client boundaries

CLI and web clients submit commands and render runtime events. Authority,
accounting and conversation mutation stay in the C++ runtime. In the browser,
`useHost` (`web/src/state/use-host.ts`) owns the subscription, `store.ts`
applies events, and components own only presentation and local interaction
state. Derive status from the shared snapshot; do not keep another
conversation or execution state machine in a component. Use semantic controls,
visible focus and accessible status text.
