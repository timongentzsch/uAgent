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
