# Browser task

Runs one prompt as a conversation of a web host with the agent's browser and
reports what the `browser` tool did with a real page: how many calls, how
long, and whether it handed over to a person.

```sh
python3 benchmarks/browser/run.py http://HOST:PORT "Open example.com and say what its heading is." [provider/model]
```

`http://HOST:PORT` is the origin the web host was started with. The optional
third argument selects the model with `/model` before the prompt is sent.

Run it on the host from a checkout (it borrows the test web client in
`tests/`), with `uagent` on the path. It pairs a temporary client and logs it
out when done; the conversation stays in `~/uagent-browser-bench`. It prints
the final status, wall time, usage, each tool result and the answer.

For a headless check of the same tool while the web host runs, point
`UAGENT_BROWSER_DATA` at that host's browser directory:

```sh
UAGENT_BROWSER_DATA=/path/to/browser-data \
  uagent -p "Open example.com and say what its heading is."
```

Signing in is part of what it measures: with a login saved in the Chrome
profile the agent uses it through `fill_saved`; without one it asks for a
person, which a headless run declines.
