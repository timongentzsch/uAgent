#!/usr/bin/env bash
# Runs before a push what CI rejects and a macOS checkout does not show:
# formatting, style, and GCC 13 with warnings as errors
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
.github/lint.sh

# What clang-tidy reads, chosen as for a pull request against BASE.
units=$(GITHUB_EVENT_NAME=pull_request CI_BASE_SHA=$base python3 .github/changes.py |
  sed -n 's/^tidy=//p')

echo "== GCC 13 -Werror build, clang-tidy (ubuntu:24.04)"
docker run --rm \
  -v "$root":/work/uagent:ro \
  -v uagent-ci-build:/work/build \
  -e UNITS="$units" \
  ubuntu:24.04 bash -euo pipefail -c '
    apt-get update -qq >/dev/null
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      g++ cmake make curl python3 libcurl4-openssl-dev zlib1g-dev libssl-dev >/dev/null
    cd /work/uagent
    cmake --preset ci-release -B /work/build/release >/dev/null
    cmake --build /work/build/release --parallel "$(nproc)" -- -k
    if [ -n "$UNITS" ]; then
      curl -LsSf https://astral.sh/uv/0.11.28/install.sh | sh >/dev/null 2>&1
      cmake --preset tidy -B /work/build/tidy >/dev/null
      files=$(python3 .github/changes.py --tidy-files /work/build/tidy "$UNITS")
      [ -z "$files" ] || "$HOME/.local/bin/uvx" --from clang-tidy==22.1.8 \
        run-clang-tidy.py -quiet -p /work/build/tidy \
        -header-filter=".*/(include|src|tests|benchmarks)/.*" $files
    fi'
echo "== all CI-only checks passed"
