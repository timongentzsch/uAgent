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

On the host that has the binary and a provider (paths are the host's):

```sh
mkdir -p ~/uagent-eval && cp -r benchmarks/delegation/. ~/uagent-eval/
git archive HEAD src include docs tests web/src skills README.md \
  SECURITY.md CHANGELOG.md CMakeLists.txt > ~/uagent-eval/corpus.tar
cd ~/uagent-eval && nohup ./all.sh &      # twelve runs, one after another
./rerun.sh                                # repeats runs a provider error ended
python3 collect.py                        # table, and results.json
```

Each run gets its own home and workspace under `runs/<task>-<mode>/`, with
the provider settings copied from the real home and the model named in
`prep.sh`. Nothing touches the real settings or conversations.

`research` and `audit` read this repository, so their answers follow the
commit the corpus was made from. One trial per cell: read differences of a
factor, not of a few percent.

## Results

Every run is in `benchmarks/baselines/delegation.json`, keyed by commit.
Coordinator runs, wall time and input tokens:

| Task | `fb55c4c6` (before) | `aea30096` | `ae26decc` |
| --- | --- | --- | --- |
| fix | 253 s / 357k | 133 s / 88k | 127 s / 105k |
| survey | 190 s / 280k | 143 s / 97k | 88 s / 54k |
| research | 255 s / 702k | 236 s / 491k | 101 s / 138k |
| audit | 231 s / 548k | 267 s / 289k | 290 s / 350k |

Before the fixes thirteen attempts ended on the provider's
`server_is_overloaded`; since then twelve of twelve complete first time. A
coordinator itself takes 3 to 12 steps; the rest is its threads' work. The
provider reports no cached input on this route, with or without a cache key.
