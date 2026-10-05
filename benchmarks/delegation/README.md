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

## Baseline, 2026-10-05, `local/gpt-6.1-sol`, commit `fb55c4c6`

| Task | Mode | Wall | Steps | Input tokens | Cached |
| --- | --- | --- | --- | --- | --- |
| fix | solo | 71 s | 5 | 25k | 0% |
| | sub | 153 s | 9 | 85k | 9% |
| | coord | 253 s | 49 | 357k | 18% |
| survey | solo | 101 s | 6 | 37k | 4% |
| | sub | 74 s | 5 | 26k | 15% |
| | coord | 190 s | 31 | 280k | 19% |
| research | solo | 128 s | 9 | 98k | 17% |
| | sub | 161 s | 15 | 336k | 9% |
| | coord | 255 s | 49 | 702k | 20% |
| audit | solo | 159 s | 11 | 206k | 0% |
| | sub | 206 s | 17 | 457k | 6% |
| | coord | 231 s | 32 | 548k | 10% |

Every answer was correct. Thirteen further attempts ended on the provider's
`server_is_overloaded`. The coordinator runs had no automatic reviewer (the
isolated homes lacked its key), so every thread action was put to the
coordinator; `prep.sh` now copies that key.
