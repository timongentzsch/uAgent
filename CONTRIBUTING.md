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
  Cancellation travels on a `std::jthread` stop token; a `std::stop_callback`
  wakes whatever the thread is blocked on.
- Own a file descriptor with `Fd` (`include/core/fd.h`), never a bare `int`
  plus a `close` on each early return. Name the mutex that covers shared state
  with `UAGENT_GUARDED_BY`, and lock-holding helpers with `UAGENT_REQUIRES`
  (`include/core/thread_annotations.h`).
- Put session-static configuration in `RuntimeConfig`. Environment accessors
  are reserved for deliberately dynamic route/delegation state. Avoid
  unbounded inputs, queues, and arithmetic.
- Never pass credentials to child processes implicitly or log secrets. Keep
  approved-shell exceptions in the centralized `ChildEnvironment` policy.
- Keep persistence versioned and validate complete temporary state before
  replacing live state.

Add focused unit coverage for local behavior or one hermetic integration path
for externally visible behavior. Avoid covering the same contract at multiple
layers. Parser changes should preserve chunk-boundary equivalence. See
[the test guide](docs/TESTING.md).

## Verify

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

uv run --frozen ruff check tests benchmarks
uv run --frozen ruff format --check tests benchmarks
git diff --check
```

Builds are warning-clean under `-Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion -Wshadow -Wold-style-cast`.
Make a narrowing or signedness change explicit at the point it happens rather
than widening the type that receives it.

clang-tidy runs whole check families, so run it before pushing. On macOS use a
Homebrew LLVM binary with the Apple SDK: upstream clang-tidy cannot parse the
SDK's libc++ headers, and without `-isysroot` neither can Homebrew's.

```sh
cmake -S . -B build/tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
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
