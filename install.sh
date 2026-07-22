#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build-install"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build" --parallel
cmake --install "$build" --prefix "$prefix"
