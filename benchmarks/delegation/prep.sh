#!/bin/bash
# prep.sh <run-name> <corpus|ledger>  -> prints nothing; creates ~/uagent-eval/runs/<run>/{home,ws}
set -e
RUN=~/uagent-eval/runs/$1
rm -rf "$RUN"; mkdir -p "$RUN/home/.uagent/config" "$RUN/ws"
python3 - "$RUN" <<PY
import json,os,sys
run=sys.argv[1]
d=json.load(open(os.path.expanduser("~/.uagent/config/settings.json")))
a=d["all"]
keep={k:a[k] for k in ("LOCAL_PROXY_API_KEY","UAGENT_PROVIDERS","OPENROUTER_API_KEY") if k in a}
keep.update({"UAGENT_MODEL":"local/gpt-6.1-sol","UAGENT_APPROVAL":"yolo","UAGENT_SANDBOX":"0",
             "UAGENT_MEMORY":"0","UAGENT_WEB_SEARCH_BACKEND":"off","UAGENT_DEBUG_LOG":run+"/debug.jsonl"})
json.dump({"format":1,"all":keep,"projects":{}},open(run+"/home/.uagent/config/settings.json","w"))
os.chmod(run+"/home/.uagent/config/settings.json",0o600)
PY
if [ "$2" = corpus ]; then tar -xf ~/uagent-eval/corpus.tar -C "$RUN/ws"; else cp -r ~/uagent-eval/ledger/. "$RUN/ws/"; fi
find "$RUN/ws" -name __pycache__ -prune -exec rm -rf {} +
cd "$RUN/ws" && git init -q && git add -A && git -c user.name=eval -c user.email=eval@local commit -qm base
