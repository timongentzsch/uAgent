#!/bin/bash
# Stops every session runtime started under the evaluation folder.
for pid in $(pgrep -f "session-worker"); do
  if tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null | grep -q "/uagent-eval/runs/"; then kill $pid 2>/dev/null; fi
done
exit 0
