#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build/release"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
build_jobs=${UAGENT_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}
# Reuse a built UI only while its source inputs match. Without npm, CMake
# can still embed a supplied bundle or build the CLI-only configuration.
if command -v npm >/dev/null 2>&1 &&
   ! node "$root/web/scripts/build-state.js" check; then
  if ! node "$root/web/scripts/build-state.js" check-dependencies; then
    npm ci --prefix "$root/web"
    node "$root/web/scripts/build-state.js" record-dependencies
  fi
  npm run build --prefix "$root/web"
fi
if [ ! -f "$build/CMakeCache.txt" ]; then
  cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
fi
cmake --build "$build" --target uagent --parallel "$build_jobs"
cmake --install "$build" --prefix "$prefix" --strip
# Chrome reads managed policy only from a root-owned directory, so this is
# printed rather than run.
if [ "$(uname)" = Linux ] && ! cmp -s "$root/deploy/chrome-policy.json" \
  /etc/opt/chrome/policies/managed/uagent.json 2>/dev/null; then
  printf '%s\n' "To block Chrome's local password pages for the web browser, run once:" \
    "  sudo install -D -m 0644 $root/deploy/chrome-policy.json /etc/opt/chrome/policies/managed/uagent.json"
fi
