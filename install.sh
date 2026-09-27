#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build/release"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
build_jobs=${UAGENT_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}
if [ ! -f "$build/CMakeCache.txt" ]; then
  cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
fi
cmake --build "$build" --target uagent --parallel "$build_jobs"
cmake --install "$build" --prefix "$prefix"
# Chrome reads managed policy only from a root-owned directory, so this is
# printed rather than run.
if [ "$(uname)" = Linux ] && ! cmp -s "$root/deploy/chrome-policy.json" \
  /etc/opt/chrome/policies/managed/uagent.json 2>/dev/null; then
  printf '%s\n' "To block Chrome's local password pages for the web browser, run once:" \
    "  sudo install -D -m 0644 $root/deploy/chrome-policy.json /etc/opt/chrome/policies/managed/uagent.json"
fi
