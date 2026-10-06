# Delegation benchmark

Four tasks, each run three ways: one session that may not delegate (`solo`),
one that may use subagents (`sub`), and a folder's coordinator (`coord`).
It measures wall time, model steps and tokens per session, and checks the
answers that can be checked mechanically.

| Task | What | Checked by |
| --- | --- | --- |
| `fix` | make the `ledger/` project's tests pass | its tests |
| `survey` | counts and purpose of ten source directories | compare with `wc` |
| `research` | how a setting's value is resolved | reading |
| `audit` | every tool and who may use it | reading |

## Running it

Run it on a Linux host that has the binary and a provider. The scripts are
written for one host: `run.sh` and `rerun.sh` expect the folder at
`/home/dev/uagent-eval` and the binary at `/home/dev/.local/bin/uagent`, and
`prep.sh` names the model (`local/gpt-6.1-sol`). Edit those for another host.

```sh
mkdir -p ~/uagent-eval && cp -r benchmarks/delegation/. ~/uagent-eval/
git archive HEAD src include docs tests web/src skills README.md \
  SECURITY.md CHANGELOG.md CMakeLists.txt > ~/uagent-eval/corpus.tar
cd ~/uagent-eval && nohup ./all.sh &      # twelve runs, one after another
./rerun.sh                                # waits, repeats unfinished runs
python3 collect.py                        # table, and results.json
```

Each run gets its own home and workspace under `runs/<task>-<mode>/`. Its
provider settings are copied from the real `~/.uagent/config/settings.json`;
it runs in yolo with the sandbox, memory and web search off. Nothing touches
the real settings or conversations. `progress.txt` shows where `all.sh` is.

`research` and `audit` read this repository, so their answers follow the
commit the corpus was made from. One trial per cell: read differences of a
factor, not of a few percent.

## Results

Every run is in `benchmarks/baselines/delegation.json`, keyed by commit.
Coordinator runs, wall time and input tokens:

| Task | `fb55c4c6` (before) | `aea30096` | `ae26decc` | `c6cff034` |
| --- | --- | --- | --- | --- |
| fix | 253 s / 357k | 133 s / 88k | 127 s / 105k | 148 s / 80k |
| survey | 190 s / 280k | 143 s / 97k | 88 s / 54k | 104 s / 52k |
| research | 255 s / 702k | 236 s / 491k | 101 s / 138k | 89 s / 102k |
| audit | 231 s / 548k | 267 s / 289k | 290 s / 350k | 356 s / 680k |

Before the fixes thirteen attempts ended on the provider's
`server_is_overloaded`; since then the runs complete first time, but for one
coordinator run at `c6cff034` that answered a spawn with empty text (the
spawn result now asks for a line). A
coordinator itself takes 3 to 12 steps; the rest is its threads' work. The
provider reports no cached input on this route, with or without a cache key.
