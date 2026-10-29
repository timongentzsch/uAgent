# Guide

From a first conversation to several running at once. Each section says what
to do and links to the reference with the detail. The [README](../README.md)
has the install and a one-screen quick start.

Terms: a **conversation** is what you talk in (its file and its running
process are a *session*); an **agent** is a conversation another one
started to do part of its work; an **activity** is a command that keeps
running in the background; **approval** is being asked before an action.

## A first conversation

```sh
cd my-project
uagent
```

Type what you want and press Enter. The agent reads files, proposes changes
and runs commands in the folder it was started in. `/help` lists every
command with its arguments.

| Input | Action |
| --- | --- |
| Enter | Send; while a turn runs, queue the draft as guidance for it |
| Shift+Enter, Alt+Enter | Newline in the draft |
| Esc | Clear the draft and stop the running turn |
| Tab | Complete a `/command` or an `@path` |
| Up, Down | Previous and next draft; in the `/` menu, move the highlight |
| Ctrl+X Ctrl+E | Edit the draft in `$VISUAL` or `$EDITOR` |
| Ctrl+A, Ctrl+E, Ctrl+W, Ctrl+K, Ctrl+U | Line editing as in a shell |
| Ctrl+B | Move the running command to the background |
| Ctrl+C | Stop a running turn; twice while idle, leave |
| Ctrl+D on an empty draft | Leave |

`/attach PATH` sends a file with the next message; `@path` in the draft
names one. `uagent --plain` prints labelled lines without animation for a
screen reader ([Accessibility](ACCESSIBILITY.md)).

## Models and providers

A model is named `[provider/]model[:effort]`, for example
`deepseek/deepseek-v4-flash:high`. A suffix that is not an effort stays in
the model id, so an endpoint's routing suffix (OpenRouter's `:nitro`,
`:floor` or `:free`) is part of the name: `openrouter/vendor/model:nitro:high`.

- `/model QUERY` searches the models your key offers and selects one; `/model
  all` lists every catalogue.
- `/model NAME` chooses for this conversation; `/model NAME --default` also
  saves it for new ones. `/effort LEVEL` sets the reasoning effort.
- With no model set, the first start opens the model picker and saves your
  choice as `/model NAME --default` does. `-p` and `--json` never choose for
  you: they exit with an error that lists up to ten of the endpoint's models.
- Background work uses your conversation's model unless you name one: the
  title (`title.model`, or `off`), memory extraction and agents. Image
  reading does too when that model accepts images; otherwise it uses the
  first model of the same endpoint that does, or tells you to set
  `image.model`.
- Web search needs hosted search on the conversation's route, or an
  endpoint with a search plugin (OpenRouter has one) and `webSearch.model`;
  see [Tools](TOOLS.md#web).
- The `auto` approval reviewer needs an endpoint that declares `decisions`
  (OpenRouter does). With none, Auto mode asks you.

`uagent setup` asks for an OpenRouter, OpenAI and Anthropic key (Enter skips
one), checks each against its endpoint, and saves them for all conversations
with a default model it suggests from the first endpoint that has a key. An
OpenAI or Anthropic key becomes a provider named `openai` or `anthropic` that
refers to it as a variable, so `anthropic/claude-sonnet-5-5` names a model
there. Run it again to change them; a start with no key offers it.

To keep a key between shells, save it once instead of exporting it. It
applies to conversations started afterwards (`/restart` applies it to this
one):

```text
/config user openrouter.apiKey=sk-or-…
```

One endpoint that speaks Chat Completions needs only `UAGENT_BASE_URL`,
`UAGENT_MODEL` and, if it asks for one, `UAGENT_API_KEY`. Anything else is a
named provider in the settings file (`~/.uagent/config/settings.json`, also
editable in the web's Settings):

```json
{
  "format": 3,
  "projects": {},
  "all": {
    "model": "work/sonnet",
    "providers": {
      "work": {
        "base_url": "https://llm.example.com/anthropic/v1",
        "api_key": "$WORK_KEY",
        "wire_api": "anthropic_messages",
        "models": { "sonnet": "claude-sonnet-5-5" }
      },
      "openrouter": {
        "options": { "provider": "Fireworks", "pdf_engine": "mistral-ocr" }
      },
      "local": {
        "base_url": "http://localhost:8080/v1",
        "context": 131072
      }
    },
    "variables": { "WORK_KEY": "…" }
  }
}
```

| Field | Meaning |
| --- | --- |
| `wire_api` | `chat_completions` (default), `responses` or `anthropic_messages` |
| `models` | Short names: `work/sonnet` selects that model at that provider; `local/anything` passes the name through |
| `context` | The context window, where the endpoint does not report one |
| `options` | Endpoint options, such as OpenRouter's upstream `provider` and document engine `pdf_engine` (`cloudflare-ai` by default) |
| `supports` | What an endpoint of OpenRouter's dialect does, from `hosted_search`, `search_plugin`, `provider_routing`, `session_passthrough`, `raw_vendor_ids`, `model_variants`, `reasoning_object`, `reasoning_replay_text`, `prompt_cache_control`, `pdf_parser`, `max_completion_tokens`, `search_sources`, `reasoning_summary`, `inline_usage` and `decisions` |
| `hosted_tools` | Tools the endpoint runs itself. `"web_search"` declares `hosted_search`, in place of uAgent's own search; an object is a tool as the endpoint's API documents it, such as `{"type": "code_execution_20250825", "name": "code_execution"}`, sent as written with the agent's requests there, in place of the agent's own tool of its `name`, and replayed with its results. A model's own list replaces the provider's |

An endpoint with an OpenRouter or OpenAI address is recognized by it. A
provider named `openrouter` needs no `base_url` or `api_key` (the key is
`OPENROUTER_API_KEY`) and states only what differs, as above.

`decisions` marks an endpoint that serves decision models for
[Auto approval](#approvals-and-the-sandbox). Its `options` give the address
(`decisions_url`), the default model (`decisions_model`), each model's bars
(`decision_bars`) and, when it also declares `provider_routing`,
`decisions_private: false` to stop asking for providers that retain nothing.

When a start must read the endpoint's `/models` for a default model or the
context window, it keeps the answer for a day under `~/.uagent/models/`.
Delete the file to ask again.

## Settings

| To | Use |
| --- | --- |
| see what is set and where from | `/config` |
| save for all conversations | `/config user KEY=VALUE` |
| save for this folder | `/config project KEY=VALUE` |
| set this conversation's model or approval mode | `/config conversation KEY=VALUE` |
| remove a saved value | `/config user unset KEY` |
| see one setting, scope by scope | `/config KEY` |

`KEY` is a setting's name (`limits.maxSteps`) or its environment name
(`UAGENT_MAX_STEPS`). A flag overrides the environment, which overrides the
folder's settings, which override those for all conversations. Nothing is
read from the folder, and `.env` files are never loaded.
[Operations](OPERATIONS.md#settings) describes the file and when a change
applies; `/config` lists every setting with its default.

## Approvals and the sandbox

Two separate mechanisms keep a turn in bounds.

- **Approval** decides who is asked. In `ask`, the default, you approve each
  file change, command, network use and read outside the folder: `y` once,
  `s` for this conversation, `a` always in this repository, `n` no. Any other
  answer denies the call and reaches the model as guidance.
  `/permissions auto` lets a decision model decide the calls the sandbox
  contains and asks you about the rest; `/permissions yolo` (or `--yolo`)
  asks nobody, and lets commands read anywhere and write your home folder
  (the `relaxed` level). A few actions always ask, such as changing
  µAgent's own settings.
- **The sandbox** decides what a command can touch, whatever the approval
  mode: it writes only inside the folder, cannot read your saved logins, and
  reads none of your other folders in the home folder (another repository,
  `~/Documents`), while the tools' own dot-folders stay readable.
  `sandbox.read` opens a folder. `sandbox.level` sets it to `readonly`,
  `default`, `relaxed` or `off`; `sandbox.network` turns the network off. A command that fails on a path outside the
  folder is the sandbox working.

**Auto approval** asks a *decision model*, a model that answers questions
with probabilities (OpenRouter serves `~typesafe/jev-latest` and
`openai/gpt-6-luna-decisions`), about each call the sandbox contains. It
never judges a command that runs outside the sandbox (or any command while
the sandbox is off or not enforced), a path outside the folder, or anything
else that always asks: those go to you.

- It sends your last message and the action. Requests ask for providers that
  keep no data (`decisions_private`) and carry a hash of the conversation,
  never its id.
- It allows when P(allow) reaches the model's allow bar and none of four risk
  questions (irreversible, beyond your request, sends data off the machine,
  touches credentials) reaches its risk bar. It refuses when P(deny) reaches
  the deny bar. Otherwise it asks you, showing the risks it saw with their
  probabilities and the bar that decided. With nobody to ask, an undecided
  call is denied.
- Automatic decisions are checked again on later calls, so a restriction you
  add later takes effect.
- The bars are part of the endpoint's data, found with `tests/reviewer_eval`.
  `approval.reviewerModel` (`[endpoint/]model`) chooses another decision
  model.

`/changes` lists the files the last turn changed and `/undo` puts them back
(in the web, Undo under the turn). Changes a shell command made are not
tracked. [Security](../SECURITY.md) has the full rules.

### Plan mode

`/plan` (in the web, Plan in the approval control) has the agent research
and write a plan before anything changes. It only reads: commands run in the
`readonly` sandbox, agents it starts only read, and any other change is
refused. The plan is a markdown file kept beside the conversation, never in
the folder. Each turn ends with the plan shared in the conversation and three
choices: **Approve and run in auto** or **Approve and ask before changes**
leave plan mode in that mode and start the work; **Keep planning** sends your
feedback for another round. A turn that writes no plan says so and the
conversation stays in plan mode, through a restart too; `/plan` again leaves
it.

## Leaving and coming back

`/quit`, Ctrl+D and closing the browser tab leave; the conversation keeps
running and stops by itself after 15 minutes with nothing to do.

| To | Use |
| --- | --- |
| continue the latest conversation | `uagent -c` |
| pick a saved one | `uagent --resume`, or `/sessions` inside |
| start over in the same folder | `/reset` |
| branch a conversation | `/fork`, or `/fork @3` at your third message |
| edit an earlier message | `/rewind N` forks before it |
| start or continue a side conversation with the same tools | `/btw QUESTION` |
| make room in a long conversation | `/compact` |

`/btw` copies the conversation's current checkpoint into a separate, saved
conversation with the parent's model, approval setting and enabled tools.
Its messages stay out of the parent's transcript, but its tools work in the
same folder and can change files or external state. In the web its card stays beside the input: follow up there,
or open it as a full conversation. Closing the card hides it; the
conversation stays in the list. In a terminal, `/btw` opens that
conversation.

[Persistence](PERSISTENCE.md) says where conversations are kept and for how
long.

## In the browser and on a phone

`uagent --web` prints a link that pairs the browser; its code works once,
for five minutes. One web host serves every folder's conversations, and a
conversation open in a terminal is the same one in the browser.
[Web interface](WEB.md) covers phones, other machines and notifications;
[The agent's browser](BROWSER.md) covers the Chrome the agent can drive.

## Agents

For work with independent parts the agent starts **agents**: conversations
of their own, each on a brief, listed under the conversation that started
them (their parent). `/agents` lists them in the terminal and `/open ID`
switches to one; in the web an agent's link opens it.
[Tools](TOOLS.md#agents) has the detail.

- **What an agent may change.** By default nothing: it reads, searches and
  runs read-only commands. For a change it works in a git worktree of its
  own under `~/.uagent/worktrees/`, started from your `HEAD`, or in the
  folder itself. To take worktree work, have it commit, then merge or
  cherry-pick that commit from your checkout.
- **Answers.** The parent usually waits for its agents' answers; Esc stops
  them. An agent it does not wait for reports when its turn ends, and the
  report starts the parent's next turn. A message to an agent starts it
  again.
- **Approvals.** An agent takes its parent's approval mode, Auto at most.
  Its approvals and questions go to its parent, which decides or passes them
  to you ([Tools](TOOLS.md#approval)); once you write into an agent's turn,
  that turn's come to you.
- **Limits.** Eight agents at work per conversation (`agents.max`), started
  two levels deep at most (`agents.depth`). An agent's spend counts in its
  parent's, and under a budget each one gets a share of what is left.

### The to-do list

For work with several parts an agent keeps a to-do list: each item pending,
in progress or done, at most one in progress. It is kept with the
conversation and shown to the agent on every request, so it survives a long
conversation, a restart and a fork; the web shows it above the input.

## A folder at a glance

In the web, a folder's name in the sidebar opens its overview: what needs
you, answered in place, and the folder's conversations with their state,
current step, to-do progress and cost, beside what they spent and changed
today ([Web interface](WEB.md#quick-start)). `/overview` prints the same in
a terminal: conversations at work, spend today and over 7 days, files
changed today, and what needs you.

## Memory, skills and schedules

The agent keeps facts between conversations as memories, loads instructions
for particular tasks as skills, and can run a task on a schedule. See
[Memory, skills and scheduled tasks](MANAGEMENT.md) and
[Bundled skills](../skills/README.md); [Tools](TOOLS.md) lists what the agent
can do.

## From a script

`-p` runs one turn and prints the answer; a failed run exits nonzero.

```sh
uagent -p "inspect this repository"
uagent -p "inspect this repository" --json          # one JSON envelope
uagent -p "inspect this repository" --json-stream   # one JSON event per line
uagent -p "fix the tests" --yolo --budget 2 --token-budget 20000
```

- `--json` prints one object with `schema` (`uagent.headless.v1`), `answer`,
  `error`, `stop`, `usage`, `routes`, `trace` and `exit_code`. `usage` and
  `routes` are what this run spent, also when `-c` continues a conversation.
  Nothing else goes to stdout; problems go to stderr, and a conversation that
  cannot be continued still gets an envelope, with `error` set.
- A `-p` run is saved like any conversation: `-c -p` continues the latest
  one in the folder, and `--resume` a named one.
- `--json-stream` prints events as they happen, each
  `{"schema": "uagent.event.v2", "seq": N, "time": "…", "type": "…",
  "data": {…}}`. Types include `turn.started`, `usage.updated`, `notice`,
  tool and approval events, and finally `answer`, whose `data` is the
  `--json` envelope.
- `--budget` caps spend in USD and `--token-budget` generated tokens. Nobody
  is there to approve, so pass `--yolo` or expect calls that need approval to
  be denied.

[Operations](OPERATIONS.md) covers limits, budgets and what to do when a run
fails.
