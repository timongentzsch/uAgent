#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build-install"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build" --parallel
cmake --install "$build" --prefix "$prefix"

# Bundled skills, installed only when the machine does not already have one by
# that name. SKILL.md is a shared format, so a skill installed for another agent
# is already on the search path — copying ours in beside it would just create a
# second copy that drifts.
skills=${UAGENT_SKILLS_DIR:-"$HOME/.uagent/skills"}
for skill in "$root"/skills/*/; do
  [ -f "$skill/SKILL.md" ] || continue
  name=$(basename "$skill")
  found=""
  for dir in "$HOME/.agents/skills" "$HOME/.claude/skills" "$HOME/.codex/skills" "$skills"; do
    [ -e "$dir/$name" ] && found="$dir/$name" && break
  done
  if [ -n "$found" ]; then
    echo "-- Skill already present, leaving it: $found"
  else
    mkdir -p "$skills"
    cp -R "$skill" "$skills/$name"
    echo "-- Installing skill: $skills/$name"
  fi
done
chmod 700 "$HOME/.uagent" "$skills" 2>/dev/null || true
