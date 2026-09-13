# Contributing

Keep µAgent small, bounded, and explicit. Prefer one shared policy or helper
over local exceptions, and preserve behavior before redesigning a boundary.

## Changes

- Add tools through `MakeTool`. Set approval, mutation, concurrency, timeout,
  result, and call limits; return a typed `ToolResult`.
- Put generic HTTP/SSE behavior in transport, OpenAI-compatible decoding in
  protocol code, and provider quirks in provider normalization.
- Route filesystem access through the shared path policy. Reject unsupported
  file types before opening them.
- Give asynchronous work one owner, a deadline, bounded output, cancellation,
  and a tested shutdown path. Do not call external code while holding a mutex.
  Use stop tokens for supervised work and the shared wake descriptor for
  socket loops; destruction wakes and joins the owning thread.
- Own a file descriptor with `Fd` (`include/core/fd.h`), never a bare `int`
  plus a `close` on each early return. Name the mutex that covers shared state
  with `UAGENT_GUARDED_BY`, and lock-holding helpers with `UAGENT_REQUIRES`
  (`include/core/thread_annotations.h`).
- Put session-static configuration in `RuntimeConfig`. Environment accessors
  are reserved for deliberately dynamic route/delegation state. Avoid
  unbounded inputs, queues, and arithmetic.
- Tool children use the centralized `ChildEnvironment` credential policy.
  Session workers inherit the trusted application environment. Never log secrets.
- Keep persistence versioned and validate complete temporary state before
  replacing live state.

Add focused unit coverage for local behavior or one hermetic integration path
for externally visible behavior. Avoid covering the same contract at multiple
layers. Parser changes should preserve chunk-boundary equivalence. See
[the test guide](docs/TESTING.md).

## Client boundaries

CLI and web submit commands and project runtime events. Keep authority, accounting
and conversation mutation in the C++ runtime. In the browser, `useHost` owns the
subscription, `store.ts` applies events, and components own only presentation and
local interaction state. Derive status from the shared snapshot; do not maintain
another conversation or execution state machine in a component.

These boundaries follow the [C++ Core Guidelines on ownership and concurrency](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#S-concurrency)
and [React's guidance on avoiding redundant state](https://react.dev/learn/choosing-the-state-structure).
Use semantic controls, visible focus and accessible status text; see
[W3C status message guidance](https://www.w3.org/WAI/WCAG22/Understanding/status-messages.html).
Tests verify specific contracts; these references are design guidance, not a
claim of complete conformance.

## Verify

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

uv run --frozen ruff check tests benchmarks skills
uv run --frozen ruff format --check tests benchmarks skills
git diff --check
```

Builds are warning-clean under `-Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion -Wshadow -Wold-style-cast`.
Make a narrowing or signedness change explicit at the point it happens rather
than widening the type that receives it.

clang-tidy runs whole check families, so run it before pushing. Configure
through the preset rather than by hand: it turns off the precompiled header,
and an `-include-pch` written by one clang is not readable by another. On
macOS use a Homebrew LLVM binary with the Apple SDK: upstream clang-tidy
cannot parse the SDK's libc++ headers, and without `-isysroot` neither can
Homebrew's.

```sh
cmake --preset tidy
$(brew --prefix llvm)/bin/run-clang-tidy \
  -clang-tidy-binary $(brew --prefix llvm)/bin/clang-tidy \
  -p build/tidy -header-filter='.*/(include|src|tests)/.*' -quiet \
  -extra-arg=-isysroot$(xcrun --show-sdk-path)
```

Before release, also run the Release preset and the sanitizer, TSan, fuzz, and
coverage CI jobs. Benchmarks are trend signals, not correctness gates.

See [architecture](docs/ARCHITECTURE.md),
[persistence](docs/PERSISTENCE.md), [operations](docs/OPERATIONS.md), and
[tools](docs/TOOLS.md).
