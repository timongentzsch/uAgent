#!/bin/bash
# Reruns every run that ended in a provider error, up to three more times each,
# keeping each failed attempt as runs/failed/<name>.<n> for the record.
cd /home/dev/uagent-eval
until grep -q "ALL DONE" progress.txt; do sleep 20; done
mkdir -p failed
for attempt in 1 2 3; do
  for task in fix survey research audit; do
    for mode in solo sub coord; do
      name=$task-$mode
      reason=$(python3 -c "import json,sys;print(json.loads(open('runs/$name/out.json').read().strip().splitlines()[-1])['stop']['reason'])" 2>/dev/null)
      if [ "$reason" != completed ]; then
        echo "$(date +%T) rerun $name (was: $reason) attempt $attempt" >> progress.txt
        rm -rf failed/$name.$attempt; cp -r runs/$name failed/$name.$attempt 2>/dev/null
        sleep 60
        ./run.sh $task $mode
        echo "$(date +%T) done  $name $(cat runs/$name/wall.txt)" >> progress.txt
      fi
    done
  done
done
echo "RERUNS DONE" >> progress.txt
