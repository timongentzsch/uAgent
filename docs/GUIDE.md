# Guide

The whole story in order, from a first conversation to several running at
once. Each section says what to do and links to the reference that has the
detail. [README](../README.md) has the install and a one-screen quick start.

Words used throughout: a **conversation** is what you talk in (its file and
its running process are a *session*); a **thread** is a conversation a
coordinator started to do work; a **member** is a persona in a coordinator's
chat; an **activity** is a command or agent that keeps running in the
background; **approval** is being asked before an action.

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

A model is named `[provider/]model[:variant][:effort]`, for example
`deepseek/deepseek-v4-flash:high`.

- `/models QUERY` searches the models your key offers and selects one.
- `/model NAME` chooses for this conversation; `/model NAME --default` also
  saves it for new ones. `/effort LEVEL` sets the reasoning effort.
- With only `OPENROUTER_API_KEY` set, a default model is used. Web search
  needs an OpenRouter route or a provider with hosted search, and the `auto`
  approval reviewer an OpenRouter key.

To keep a key between shells, save it once instead of exporting it:

```text
/config user openrouter.apiKey=sk-or-…
```

It applies to conversations started after that (`/restart` for this one).

One endpoint that speaks Chat Completions needs only `UAGENT_BASE_URL`,
`UAGENT_MODEL` and, if it asks for one, `UAGENT_API_KEY`. Anything else is a
named provider in the settings file (`~/.uagent/config/settings.json`, also
editable in the web's Settings):

```json
{
  "format": 2,
  "projects": {},
  "all": {
    "model": "work/sonnet",
    "providers": {
      "work": {
        "base_url": "https://llm.example.com/anthropic/v1",
        "api_key": "$WORK_KEY",
        "wire_api": "anthropic_messages",
        "protocol": "anthropic",
        "models": { "sonnet": "claude-sonnet-5-5" }
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

`wire_api` is `chat_completions` (the default), `responses` or
`anthropic_messages`. `models` gives short names; `work/sonnet` then selects
that model at that provider, and `local/anything` passes the name through.
`context` states the context window where the endpoint does not report one.

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
(`UAGENT_MAX_STEPS`); both work everywhere. A flag overrides the
environment, which overrides what is saved for the project, which overrides
what is saved for all conversations. Nothing is read from the project
folder and `.env` files are never loaded.
[Operations](OPERATIONS.md#settings) describes the file;
the [configuration reference](../skills/uagent-config/references/configuration.md)
lists every setting with its default.

## Approvals and the sandbox

Two separate things keep a turn in bounds.

- **Approval** decides who is asked. In `ask`, the default, you approve each
  change to a file, each command, each use of the network and each read
  outside the folder: `y` once, `s` for this conversation, `a` always in this
  repository, `n` no. Any other answer denies the call and is given to the
  model as guidance. `/permissions auto` lets a reviewer model decide and
  `/permissions yolo` (or `--yolo`) asks nobody. A few actions always ask,
  such as changing µAgent's own settings.
- **The sandbox** decides what a command can touch: it writes only inside
  the folder, whatever the approval mode, unless you switch it off
  (`sandbox.enabled`). A command that fails on a path outside the folder is
  the sandbox working.

`/changes` lists the files the last turn changed and `/undo` puts them back
(in the web, Undo under the turn); changes a shell command made are not
tracked. [Security](../SECURITY.md) has
the full rules.

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
| ask something without adding it to the conversation | `/btw QUESTION` |
| make room in a long conversation | `/compact` |

[Persistence](PERSISTENCE.md) says where conversations are kept and for how
long.

## In the browser and on a phone

```sh
uagent --web
```

prints a link that pairs the browser; the code in it works once, for five
minutes. One web host serves every folder's conversations, and a
conversation open in a terminal is the same one in the browser.
[Web interface](WEB.md) covers a phone or another machine, notifications and
the browser the agent can drive.

## The coordinator

```sh
uagent coord
```

opens the folder's coordinator (`/coord` from inside a conversation; in the
web, *New conversation → Open its coordinator*). It never edits files
itself. Ask it for work and it starts **threads**: conversations of their
own, each on a brief, that report back when their turn ends. The coordinator
answers the approvals a thread cannot settle or passes them to you.

- **The board** lists the folder's conversations, what each is doing and what
  waits on you: `/board` in the terminal, beside the chat in the web.
  `/open ID` switches to one.
- **Where a thread works.** By default in a git worktree of its own under
  `~/.uagent/worktrees/`, started from your `HEAD`, so your checkout stays as
  it is while several threads work. Ask the coordinator for a thread's diff.
  To take the work, have the thread commit and merge or cherry-pick that
  commit from your checkout (the worktree shares your repository). With
  `coordinator.environment=local` threads work in the folder itself.
- **Limits.** Five threads at once (`coordinator.maxThreads`) and $20 of
  reported cost a day for a coordinator and its threads
  (`coordinator.dailySpendUsd`); at the limit, threads' reports wait.
- **Instructions.** `COORDINATOR.md` holds what every coordinator reads at
  start ([Instructions](SYSTEM_PROMPTS.md)).

### The chat

Ask the coordinator for another voice ("add a sceptical reviewer called
Ada") and it adds a **member**: a persona with a name, optionally its own
model, that reads and searches but changes nothing. From then on the
coordinator's conversation is a chat.

- **Everyone reads every message** and decides whether to answer. A
  participant with nothing to add says nothing, and one that sees someone
  else typing may wait for them. You only see the messages.
- **To ask one participant alone, start the message with its name** followed
  by a comma or a colon: `Ada, is this safe?`, `Ada and Lin: your view?`,
  `Coordinator, summarise`. Only those named answer, and the answer comes
  back to you without waking the others. A name anywhere else in the
  message addresses nobody.
- **A message for everyone** starts with no name; members also answer one
  another.
- **Limits.** For each message of yours, every participant may take three
  turns (`coordinator.chatTurns`) and post two messages; a chat has at most
  16 members.
- **Who said what.** Each message is shown under its author's name; in the
  web, a message's menu shows the prompt its author was given.
- Ask the coordinator to remove a member; it asks you to confirm.

## Memory, skills and schedules

The agent keeps facts between conversations as memories, loads instructions
for particular tasks as skills, and can run a task on a schedule.
[Memory, skills and scheduled tasks](MANAGEMENT.md) and
[Bundled skills](../skills/README.md) cover them; [Tools](TOOLS.md) lists
what the agent can do.

## From a script

`-p` runs one turn and prints the answer; a failed run exits nonzero.

```sh
uagent -p "inspect this repository"
uagent -p "inspect this repository" --json          # one JSON envelope
uagent -p "inspect this repository" --json-stream   # one JSON event per line
uagent -p "fix the tests" --yolo --budget 2 --token-budget 20000
uagent coord -p "status of the threads"             # ask the coordinator
```

- `--json` prints one object with `schema` (`uagent.headless.v1`), `answer`,
  `error`, `stop`, `usage`, `routes`, `trace` and `exit_code`. Nothing else
  is written to stdout; problems go to stderr.
- `--json-stream` prints events as they happen, each
  `{"schema": "uagent.event.v2", "seq": N, "time": "…", "type": "…",
  "data": {…}}`. Types include `turn.started`, `usage.updated`, `notice`,
  tool and approval events, and at the end `answer`, whose `data` is the
  `--json` envelope.
- `--budget` caps spend in USD and `--token-budget` generated tokens; nobody
  is there to approve, so choose `--yolo` or expect calls that need approval
  to be denied.

[Operations](OPERATIONS.md) covers limits, budgets and what to do when a run
fails.
