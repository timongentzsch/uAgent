#!/usr/bin/env bash
# Runs before a push what CI rejects and a macOS checkout does not show:
# formatting, style, generated references, and GCC 13 with warnings as errors
# plus clang-tidy on the changed sources, inside the Ubuntu image CI uses.
#
#   .github/ci-local.sh [BASE]   # BASE defaults to origin/master
#
# Needs uv and Docker.
# The container keeps its build tree in a named volume, so a second run only
# recompiles what changed.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
base=${1:-origin/master}

echo "== clang-format, cpplint, ruff"
uvx --from clang-format==22.1.8 clang-format \
  --dry-run --Werror include/*.h include/*/*.h src/*.cc src/*/*.cc \
  tests/unit/*.h tests/unit/*.cc tests/fuzz/*.cc benchmarks/*.cc
uvx --from cpplint==2.0.2 cpplint --quiet \
  --recursive --exclude=third_party --exclude=tests/fixtures \
  --extensions=h,cc \
  --filter=-build/c++17,-build/header_guard,-whitespace/indent_namespace,-readability/check \
  include src tests benchmarks
uv run --frozen ruff check .github tests benchmarks skills
uv run --frozen ruff format --check .github tests benchmarks skills

# The sources clang-tidy should see: changed translation units, and every
# unit when a header changed.
changed=$(git diff --name-only --diff-filter=d "$base" -- src tests benchmarks include)
if grep -q '\.h$' <<<"$changed"; then
  tidy_files='/(src|tests|benchmarks)/'
else
  tidy_files=$(grep '\.cc$' <<<"$changed" | sed 's|^|/work/uagent/|; s|$|$|' | tr '\n' ' ' || true)
fi

echo "== GCC 13 -Werror build, generated references, clang-tidy (ubuntu:24.04)"
docker run --rm \
  -v "$root":/work/uagent:ro \
  -v uagent-ci-build:/work/build \
  -e TIDY_FILES="$tidy_files" \
  ubuntu:24.04 bash -euo pipefail -c '
    apt-get update -qq >/dev/null
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      g++ cmake make curl python3 libcurl4-openssl-dev libssl-dev >/dev/null
    cmake -S /work/uagent -B /work/build/release -DCMAKE_BUILD_TYPE=Release \
      -DUAGENT_WARNINGS_AS_ERRORS=ON -DUAGENT_WEB_PUSH=ON >/dev/null
    cmake --build /work/build/release --parallel "$(nproc)" -- -k
    rm -rf /tmp/references
    /work/build/release/uagent --emit-reference /tmp/references
    diff -ru /work/uagent/skills/uagent-config/references /tmp/references \
      --exclude=self-configuration.md
    if [ -n "$TIDY_FILES" ]; then
      curl -LsSf https://astral.sh/uv/install.sh | sh >/dev/null 2>&1
      cd /work/uagent
      cmake --preset tidy -B /work/build/tidy >/dev/null
      "$HOME/.local/bin/uvx" --from clang-tidy==22.1.8 run-clang-tidy.py \
        -quiet -p /work/build/tidy \
        -header-filter=".*/(include|src|tests|benchmarks)/.*" $TIDY_FILES
    fi'
echo "== all CI-only checks passed"
