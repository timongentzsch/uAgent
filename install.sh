#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build-install"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build" --parallel
cmake --install "$build" --prefix "$prefix"

# Bundled skills, installed once. An existing directory is left alone so local
# edits survive an upgrade; delete one to get the shipped version back.
skills=${UAGENT_SKILLS_DIR:-"$HOME/.uagent/skills"}
for skill in "$root"/skills/*/; do
  [ -f "$skill/SKILL.md" ] || continue
  name=$(basename "$skill")
  if [ -e "$skills/$name" ]; then
    echo "-- Keeping existing skill: $skills/$name"
  else
    mkdir -p "$skills"
    cp -R "$skill" "$skills/$name"
    echo "-- Installing skill: $skills/$name"
  fi
done
chmod 700 "$HOME/.uagent" "$skills" 2>/dev/null || true
