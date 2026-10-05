# Browser task

One prompt, run as a conversation of a web host that has the browser
appliance, to see what the `browser` tool does with a real page: how many
calls, how long, and whether it had to hand over to a person.

```sh
python3 benchmarks/browser/run.py http://HOST:PORT "Open example.com and say what its heading is." [provider/model]
```

Run it on the host, with `uagent` on the path and this repository's `tests/`
beside it (it borrows the test web client). It pairs a temporary client and
logs it out when done; the conversation stays in `~/uagent-browser-bench`.

For a headless check of the same tool, with the web host running:

```sh
UAGENT_BROWSER_DATA=~/.uagent/browser uagent -p "Open example.com and say what its heading is."
```

Signing in is part of what it measures: with a login saved in the browser's
Chrome profile the agent takes it with `fill_saved`; without one it asks for
a person, which a headless run declines.
