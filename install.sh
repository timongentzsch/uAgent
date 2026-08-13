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
  dest="$skills/$name"
  src="$staging/$name"
  cp -R "$skill" "$src"
  if [ ! -e "$dest" ]; then
    mv "$src" "$dest"
    echo "-- Installing bundled skill: $dest"
    continue
  fi
  backup=$(mktemp -d "${skills}/.replace.XXXXXX")
  if mv "$dest" "$backup/old" && mv "$src" "$dest"; then
    rm -rf "$backup"
    echo "-- Refreshing bundled skill: $dest"
    continue
  fi
  if [ ! -e "$dest" ] && [ -e "$backup/old" ]; then
    mv "$backup/old" "$dest" || true
  fi
  rm -rf "$backup"
  echo "cannot install bundled skill: $dest" >&2
  exit 1
done
chmod 700 "$HOME/.uagent" "$skills" 2>/dev/null || true
