# Contributing

Keep µAgent small, bounded and explicit. Prefer one shared policy or helper
over local exceptions, and preserve behavior before redesigning a boundary.
First-party C++ follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

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

## Verify

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

uv run --frozen ruff check .github tests benchmarks skills
uv run --frozen ruff format --check .github tests benchmarks skills
git diff --check
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
```

Commit the rebuilt `web/dist` and `web/THIRD_PARTY_NOTICES.md`; CI fails when
either differs from a fresh build.

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
