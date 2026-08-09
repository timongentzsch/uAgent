#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build-install"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
build_jobs=${UAGENT_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build" --parallel "$build_jobs"
cmake --install "$build" --prefix "$prefix"

# Refresh µAgent's bundled skills on every install. The runtime loads this
# directory after user-level vendor directories, so release-matched procedures
# override incompatible copies while workspace skills can still override them.
skills=${UAGENT_SKILLS_DIR:-"$HOME/.uagent/skills"}
mkdir -p "$skills"
staging=$(mktemp -d "${skills}/.install.XXXXXX")
trap 'rm -rf "$staging"' EXIT HUP INT TERM
for skill in "$root"/skills/*/; do
  [ -f "$skill/SKILL.md" ] || continue
  name=$(basename "$skill")
  cp -R "$skill" "$staging/$name"
  if [ -d "$skills/$name" ]; then
    rm -rf "$skills/$name"
    echo "-- Refreshing bundled skill: $skills/$name"
  else
    echo "-- Installing bundled skill: $skills/$name"
  fi
  mv "$staging/$name" "$skills/$name"
done
rm -rf "$staging"
trap - EXIT HUP INT TERM
chmod 700 "$HOME/.uagent" "$skills" 2>/dev/null || true
