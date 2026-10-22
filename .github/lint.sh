#!/usr/bin/env bash
# The formatting and style checks CI runs on every change: clang-format and
# cpplint for C++, Ruff for Python. Needs uv.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
uvx --from clang-format==22.1.8 clang-format --dry-run --Werror \
  include/*/*.h src/*.cc src/*/*.cc \
  tests/unit/*.h tests/unit/*.cc tests/fuzz/*.cc benchmarks/*.cc
uvx --from cpplint==2.0.2 cpplint --quiet \
  --recursive --exclude=third_party --exclude=tests/fixtures \
  --extensions=h,cc \
  --filter=-build/c++17,-build/header_guard,-whitespace/indent_namespace,-readability/check \
  include src tests benchmarks
uv run --frozen ruff check .github tests benchmarks skills
uv run --frozen ruff format --check .github tests benchmarks skills
