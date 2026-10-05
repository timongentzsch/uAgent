#!/bin/bash
# run.sh <task: research|survey|fix|audit> <mode: solo|sub|coord>
# One evaluation run in its own home and workspace. Leaves in runs/<task>-<mode>/:
#   out.json (the headless envelope), err.txt, wall.txt (seconds), debug.jsonl, home/, ws/
task=$1; mode=$2; name=$task-$mode
E=/home/dev/uagent-eval; R=$E/runs/$name
$E/stop.sh; sleep 2
kind=corpus; [ "$task" = fix ] && kind=ledger
$E/prep.sh $name $kind || exit 1
prompt=$(cat $E/prompts/$task.txt)
cd $R/ws
export HOME=$R/home
start=$(date +%s.%N)
case $mode in
  solo)  UAGENT_TOOL_CAPABILITIES=inspect,execute,mutate,external \
         timeout 2400 /home/dev/.local/bin/uagent --json -p "$prompt" > ../out.json 2> ../err.txt ;;
  sub)   timeout 2400 /home/dev/.local/bin/uagent --json -p "$prompt

You may delegate independent parts to subagents and run them in parallel where that helps." > ../out.json 2> ../err.txt ;;
  coord) timeout 2400 /home/dev/.local/bin/uagent coord --json -p "$prompt" > ../out.json 2> ../err.txt ;;
esac
echo "$? $(echo "$(date +%s.%N) - $start" | bc)" > ../wall.txt
# Let anything still running settle, then record when the last session went quiet.
sleep 20
ls -la --time-style=+%s $R/home/.uagent/history/*/*.json 2>/dev/null | awk '{print $6, $7}' > ../sessions.txt
[ "$task" = fix ] && (cd $R/ws && python3 -m unittest discover -s tests > ../tests.txt 2>&1; git status --short > ../changed.txt; git diff --stat -- tests >> ../changed.txt)
$E/stop.sh
