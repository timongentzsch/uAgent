# Guide

From a first conversation to several running at once. Each section says what
to do and links to the reference with the detail. The [README](../README.md)
has the install and a one-screen quick start.

Terms: a **conversation** is what you talk in (its file and its running
process are a *session*); a **thread** is a conversation a panel started to do
work; a **subagent** is a short-lived helper that answers one question
inside a turn; an **activity** is a command or agent that keeps running in
the background; **approval** is being asked before an action.

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

- `/models QUERY` searches the models your key offers and selects one.
- `/model NAME` chooses for this conversation; `/model NAME --default` also
  saves it for new ones. `/effort LEVEL` sets the reasoning effort.
- With no model set, the first start opens the model picker and saves your
  choice as `/model NAME --default` does. `-p` and `--json` never choose for
  you: they exit with an error that lists up to ten of the endpoint's models.
- Background jobs use your conversation's model unless you name one: the
  title (`title.model`, or `off`), memory extraction and subagents. Image
  reading does too when that model accepts images; otherwise it uses the
  first model of the same endpoint that does, or tells you to set
  `image.model`.
- Web search needs hosted search on the conversation's route, or an
  endpoint with a search plugin (OpenRouter has one) and `webSearch.model`;
  see [Tools](TOOLS.md#web).
- The `auto` approval reviewer needs an endpoint that declares `decisions`
  (OpenRouter does). With none, Auto mode asks you.

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
| save for this project folder | `/config project KEY=VALUE` |
| set this conversation's model or approval mode | `/config conversation KEY=VALUE` |
| remove a saved value | `/config user unset KEY` |
| move settings between hosts | `uagent config export`, `uagent config import FILE` |

`KEY` is a setting's name (`limits.maxSteps`) or its environment name
(`UAGENT_MAX_STEPS`). A flag overrides the environment, which overrides the
project's settings, which override those for all conversations. Nothing is
read from the project folder, and `.env` files are never loaded.
[Operations](OPERATIONS.md#settings) describes the file and when a change
applies; the
[configuration reference](../skills/uagent-config/references/configuration.md)
lists every setting with its default.

## Approvals and the sandbox

Two separate mechanisms keep a turn in bounds.

- **Approval** decides who is asked. In `ask`, the default, you approve each
  file change, command, network use and read outside the folder: `y` once,
  `s` for this conversation, `a` always in this repository, `n` no. Any other
  answer denies the call and reaches the model as guidance.
  `/permissions auto` lets a decision model decide the calls the sandbox
  contains and asks you about the rest; `/permissions yolo` (or `--yolo`)
  asks nobody. A few actions always ask, such as changing µAgent's own
  settings.
- **The sandbox** decides what a command can touch, whatever the approval
  mode: it writes only inside the folder and cannot read your saved logins.
  `sandbox.level` sets it to `strict`, `default`, `relaxed` or `off`. A
  command that fails on a path outside the folder is the sandbox working.

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
  `approval.allowAbove`, `approval.denyAbove` and `approval.vetoAbove`
  override them; `approval.reviewerModel` (`[endpoint/]model`) chooses
  another decision model.

`/changes` lists the files the last turn changed and `/undo` puts them back
(in the web, Undo under the turn). Changes a shell command made are not
tracked. [Security](../SECURITY.md) has the full rules.

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
conversation with the parent's model, approval setting and enabled tools
(including a moderator's panel tools). Its messages stay out of the parent's
transcript, but its tools work in the same folder and can change files or
external state. In the web its card stays beside the input: follow up there,
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

## The panel

`uagent panel` opens the folder's panel (`/panel` from inside a
conversation; in the web, the place's name in the sidebar). You talk to its
**moderator**, which never edits files itself. Ask it for work and it starts
**threads**: conversations of their own, each on a brief, that report back
when their turn ends. The moderator answers the approvals a thread cannot
settle or passes them to you.

- **The board** lists the folder's conversations, what each is doing and what
  waits on you: `/board` in the terminal, beside the chat in the web.
  `/open ID` switches to one.
- **Where a thread works.** By default in a git worktree of its own under
  `~/.uagent/worktrees/`, started from your `HEAD`, so your checkout stays as
  it is while several threads work. Ask the moderator for a thread's diff. To
  take the work, have the thread commit, then merge or cherry-pick that
  commit from your checkout (the worktree shares your repository). With
  `panel.environment=local`, or in a folder outside git such as a workspace,
  threads work in the folder itself.
- **Limits.** Five threads at once (`panel.maxThreads`) and $20 of reported
  cost a day for a panel and its threads (`panel.dailySpendUsd`), counting
  what threads from earlier days spend today; at the limit, thread reports
  wait. A route that reports no cost cannot reach it, and the panel says so.
- **Instructions.** Every moderator reads `PANEL.md` at start
  ([Instructions](INSTRUCTIONS.md)).

### Questions and the plan

For a question rather than a change ("which of these three libraries fits
us?", "where can this proxy leak a key?") the moderator asks **subagents**:
short-lived helpers that read, search and fetch but change nothing, several
at once, each answering in the shape the moderator asked for. It can have
other subagents try to refute what they found, and reports only what holds.
They are not conversations of their own: open one from the plan or from its
row in the moderator's answer.

- **The plan.** For work with several parts the moderator keeps a plan:
  your goal, decisions, open questions, and steps with their status and a
  short result each. It sees the plan every turn, so it survives a long
  conversation and a restart; a thread or subagent started for a step is
  shown with it.
- **Threads or subagents.** Threads change files and run for as long as the
  work takes, in their own worktree; subagents answer within the
  moderator's turn and leave nothing behind but their answer.

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
uagent panel -p "status of the threads"             # ask the panel
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
