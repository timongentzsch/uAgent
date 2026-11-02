#!/usr/bin/env bash
# The checks CI runs on every change that need no build: clang-format and
# cpplint for C++, Ruff for Python, and the source contracts. Needs uv.
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
python3 tests/ci_changes_test.py
python3 tests/boundary_test.py
python3 tests/wire_contract_test.py
python3 benchmarks/session_metrics.py --self-test
python3 benchmarks/eval.py --self-test
