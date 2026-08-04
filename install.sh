#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build-install"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
build_jobs=${UAGENT_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build" --parallel "$build_jobs"
cmake --install "$build" --prefix "$prefix"

# External skills win by name. Refresh our own bundled copy on each install so
# its reference stays matched to the binary release.
skills=${UAGENT_SKILLS_DIR:-"$HOME/.uagent/skills"}
for skill in "$root"/skills/*/; do
  [ -f "$skill/SKILL.md" ] || continue
  name=$(basename "$skill")
  found=""
  for dir in "$HOME/.agents/skills" "$HOME/.claude/skills" "$HOME/.codex/skills"; do
    [ -e "$dir/$name" ] && found="$dir/$name" && break
  done
  if [ -n "$found" ]; then
    echo "-- Skill already present, leaving it: $found"
  else
    mkdir -p "$skills"
    if [ -d "$skills/$name" ]; then
      cp -R "$skill/." "$skills/$name/"
      echo "-- Refreshing bundled skill: $skills/$name"
    else
      cp -R "$skill" "$skills/$name"
      echo "-- Installing skill: $skills/$name"
    fi
  fi
done
chmod 700 "$HOME/.uagent" "$skills" 2>/dev/null || true
