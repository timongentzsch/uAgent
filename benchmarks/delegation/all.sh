#!/bin/bash
cd ~/uagent-eval
for task in fix survey research audit; do
  for mode in solo sub coord; do
    echo "$(date +%T) start $task-$mode" >> progress.txt
    ./run.sh $task $mode
    echo "$(date +%T) done  $task-$mode $(cat runs/$task-$mode/wall.txt)" >> progress.txt
  done
done
echo "ALL DONE" >> progress.txt
