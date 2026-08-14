# µAgent configuration reference

Configuration files contain shell-style `KEY=value` lines with optional
`export`, quotes, comments, and `$VAR`/`${VAR}` interpolation. Precedence is:
process environment > trusted `./.uagent/.config` > `~/.uagent/.config` >
built-in default. Set `UAGENT_CONFIG_FILE` to use one explicit file instead.
Project `.env` files are never imported. A project file and `.mcp.json` require
`--trust-project-config` or `UAGENT_TRUST_PROJECT_CONFIG=1`, and changes revoke
the saved trust decision.

Sizes are bytes unless stated otherwise. Empty means unset or inferred.

## Provider and session

| Variable | Default | Purpose |
| --- | --- | --- |
| `UAGENT_BASE_URL` | inferred | OpenAI-compatible API base URL |
| `UAGENT_API_KEY` | `sk-noop` | API credential |
| `UAGENT_MODEL` | last selection | model or named `provider/model` route |
| `UAGENT_REASONING_EFFORT` | empty | model effort: `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, or `max` |
| `UAGENT_CONTEXT` | provider/profile | context-window tokens |
| `UAGENT_PROVIDERS` | empty | JSON object defining named endpoints and model aliases |
| `UAGENT_OPENROUTER_COMPATIBLE` | URL-derived | force OpenRouter-compatible request behavior with `1` or `0` |
| `OPENROUTER_API_KEY` | empty | selects built-in OpenRouter route |
| `OPENROUTER_MODEL` | `openrouter/auto` | OpenRouter model when `UAGENT_MODEL` is absent |
| `OPENROUTER_EFFORT` | empty | OpenRouter effort when the UAGENT effort is absent |
| `UAGENT_OPENROUTER_PROVIDER` | empty | OpenRouter provider-routing preference |
| `UAGENT_OPENROUTER_FALLBACKS` | `1` | allow OpenRouter provider fallbacks |
| `UAGENT_OPENROUTER_VARIANT` | empty | routing mode: `nitro`, `floor`, or `exacto` |
| `UAGENT_APPROVAL` | prompt | set `yolo` for non-interactive tool approval |
| `UAGENT_SESSION_BUDGET` | `0` | reported-cost session limit in USD; `0` disables |
| `UAGENT_MAX_TURN_COST` | `0` | reported-cost limit per turn in USD; `0` disables |
| `UAGENT_MAX_TOKENS` | `16000` | maximum response tokens |
| `UAGENT_STEERING` | `1` | enable Escape foreground interruption |
| `UAGENT_ADAPT_SYSTEM` | `0` | expose experimental free-form mutable system-directive tool |
| `UAGENT_MARKDOWN` | `1` | render terminal Markdown |
| `UAGENT_DEBUG_LOG` | empty | default debug JSONL path |
| `UAGENT_USAGE_FILE` | internal | append-only usage ledger used by supervised children |

Example named routes:

```sh
UAGENT_PROVIDERS='{"local":{"base_url":"http://127.0.0.1:8000/v1","api_key":"sk-noop","context":131072,"models":{"fast":{"id":"model-id","effort":"low"}}}}'
UAGENT_MODEL=local/fast
```

## Turn, request, and context limits

| Variable | Default | Purpose |
| --- | ---: | --- |
| `UAGENT_FIRST_EVENT_TIMEOUT` | 300 | seconds to first streamed event |
| `UAGENT_STREAM_IDLE_TIMEOUT` | 300 | seconds between streamed events |
| `UAGENT_REQUEST_TIMEOUT` | 600 | complete request seconds |
| `UAGENT_MAX_TURN_SECONDS` | 3600 | complete turn seconds |
| `UAGENT_MAX_STEPS` | 0 | model rounds per turn; 0 disables |
| `UAGENT_MAX_TOOL_CALLS` | 0 | tool calls per turn; 0 disables |
| `UAGENT_REQUEST_BYTES` | 67108864 | maximum serialized request |
| `UAGENT_RESPONSE_BYTES` | 33554432 | maximum response |
| `UAGENT_AUTO_COMPACT_PCT` | 85 | context-window compaction threshold |
| `UAGENT_AUTO_COMPACT_TOKENS` | 0 | optional absolute soft compaction ceiling; 0 disables |
| `UAGENT_PROJECT_DOC_BYTES` | 32768 | startup instruction budget |
| `UAGENT_SESSION_ARCHIVE_BYTES` | 16777216 | removed-trace archive bound |

## Tools, files, processes, and delegation

| Variable | Default | Purpose |
| --- | ---: | --- |
| `UAGENT_TOOL_RESULT_CHARS` | 8000 | one tool result sent to the model |
| `UAGENT_TOOL_TRACE_PROTECT_CHARS` | 65536 | recent tool trace protected from pruning |
| `UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS` | 32768 | minimum incremental prune batch; `0` disables |
| `UAGENT_TOOL_CONCURRENCY` | 4 | safe foreground tool workers |
| `UAGENT_TOOL_TIMEOUT` | 30 | default tool seconds; `0` disables |
| `UAGENT_TOOLSET` | full | `lean` for delegated reduced tools |
| `UAGENT_TOOL_CAPABILITIES` | all | comma list: `inspect,execute,mutate,delegate,external` |
| `UAGENT_TOOL_ALLOWLIST` | empty | JSON string array of allowed tool names |
| `UAGENT_TOOL_RUN_ALLOWLIST` | empty | JSON string array of exact allowed commands |
| `UAGENT_READ_FILE_LINES` | 1000 | default source lines returned |
| `UAGENT_READ_FILE_MAX_LINES` | 10000 | maximum requested source lines |
| `UAGENT_READ_FILE_BYTES` | 32768 | source-read byte bound |
| `UAGENT_EDIT_FILE_BYTES` | 10485760 | editable file bound |
| `UAGENT_LIST_DIR_ENTRIES` | 1000 | returned directory entries |
| `UAGENT_LIST_DIR_SCAN_ENTRIES` | 100000 | scanned directory entries |
| `UAGENT_GREP_RESULTS` | 200 | grep matches |
| `UAGENT_GREP_BYTES` | tool-result cap | grep result bytes |
| `UAGENT_BASH_LOG_BYTES` | 67108864 | bounded rotating process log |
| `UAGENT_RUN_YIELD_MS` | 10000 | default initial wait for public `run`; 0 disables automatic yielding |
| `UAGENT_MAX_BACKGROUND_JOBS` | 8 | supervised live-activity slots |
| `UAGENT_SHELL_ENV_ALLOW` | empty | comma list of sensitive vars forwarded only to approved shell commands |
| `UAGENT_SUBAGENT_DEPTH` | 2 | maximum delegation depth |
| `UAGENT_SUBAGENT_MAX_STEPS` | 25 | model rounds per delegated child |
| `UAGENT_SUBAGENT_MAX_TOOL_CALLS` | 60 | tool calls per delegated child |
| `UAGENT_TASK_MODEL` | current route | default delegated model/route |
| `UAGENT_DEPTH` | internal `0` | current supervised child depth |

## Skills and memory

| Variable | Default | Purpose |
| --- | ---: | --- |
| `UAGENT_SKILL_PATH` | discovery roots | colon-separated replacement roots |
| `UAGENT_SKILL_EXCLUDE` | empty | comma-separated skill names to hide |
| `UAGENT_SKILLS` | 64 | maximum discovered skills |
| `UAGENT_SKILL_BYTES` | 524288 | complete selected skill-body bound |
| `UAGENT_SKILL_DESC_BYTES` | 1024 | startup description bound per skill |
| `UAGENT_MEMORY` | `1` | enable memory recall |
| `UAGENT_MEMORY_GENERATE` | `1` | enable idle-session background extraction |
| `UAGENT_MEMORY_BYTES` | 2048 | one memory file |
| `UAGENT_MEMORY_ALWAYS_BYTES` | 2048 | global-memory content inlined into startup context; `0` disables |
| `UAGENT_MEMORY_EXTRACT_BYTES` | 32768 | filtered transcript sent to the extractor |
| `UAGENT_MEMORY_FILES` | 32 | memories per scope |
| `UAGENT_MEMORY_IDLE_SECONDS` | 21600 | minimum saved-session idle time |

## Search, MCP, and media

| Variable | Default | Purpose |
| --- | ---: | --- |
| `UAGENT_WEB_SEARCH_SERVER` | `1` | enable provider server search |
| `UAGENT_WEB_SEARCH_BACKEND` | `auto` | search backend selection |
| `UAGENT_WEB_SEARCH_URL` | empty | custom search endpoint |
| `UAGENT_WEB_SEARCH_API_KEY` | empty | custom search credential |
| `UAGENT_WEB_SEARCH_MODEL` | empty | search model override |
| `UAGENT_WEB_SEARCH_EFFORT` | empty | search reasoning effort |
| `UAGENT_WEB_SEARCH_ENGINE` | `auto` | engine selection |
| `UAGENT_WEB_SEARCH_CONTEXT_SIZE` | empty | provider search context size |
| `UAGENT_WEB_SEARCH_TIMEOUT` | 25 | search seconds |
| `UAGENT_WEB_SEARCH_MAX_TOKENS` | 1200 | search response tokens |
| `UAGENT_WEB_SEARCH_CALLS` | 4 | search calls per tool invocation |
| `UAGENT_WEB_SEARCH_MAX_RESULTS` | 5 | results per search call, maximum 25 |
| `UAGENT_WEB_SEARCH_MAX_USES` | 3 | search tool uses per turn, maximum 30 |
| `UAGENT_MCP_TIMEOUT` | 60 | MCP operation seconds |
| `UAGENT_MCP_SERVERS` | 32 | server bound |
| `UAGENT_MCP_PAGES` | 100 | discovery page bound |
| `UAGENT_MCP_TOOLS` | 256 | registered tool bound |
| `UAGENT_MCP_CONFIG_BYTES` | 1048576 | config bound |
| `UAGENT_MCP_RESPONSE_BYTES` | 16777216 | response bound |
| `UAGENT_MCP_SCHEMA_BYTES` | 262144 | schema bound |
| `UAGENT_MCP_LOG_BYTES` | 16777216 | log bound |
| `UAGENT_MCP_ROOTS` | current workspace | colon-separated roots advertised to MCP servers |
| `UAGENT_MCP_DESC_CHARS` | 400 | tool-description bound |
| `UAGENT_ATTACHMENT_MB` | 10 | input attachment MiB |
| `UAGENT_PENDING_ATTACHMENTS` | 8 | queued attachment count |
| `UAGENT_IMAGE_MODEL` | empty | vision-capable fallback model for routes that reject image input |
| `UAGENT_IMAGE_DETAIL` | `auto` | provider input-image detail |
| `UAGENT_IMAGE_PROTOCOL` | detected | terminal image protocol override |
| `UAGENT_TERMINAL_IMAGE_MB` | 10 | rendered image MiB |
| `UAGENT_IMAGE_MAX_COLUMNS` | 200 | rendered image column cap |
| `UAGENT_IMAGE_COLUMNS` | available width | rendered image columns |

## Retention and local helpers

| Variable | Default | Purpose |
| --- | ---: | --- |
| `UAGENT_HISTORY_DAYS` / `UAGENT_HISTORY_FILES` | 30 / 200 | session retention |
| `UAGENT_DEBUG_DAYS` / `UAGENT_DEBUG_FILES` | 14 / 50 | debug-trace retention |
| `UAGENT_BG_DAYS` / `UAGENT_BG_FILES` | 7 / 200 | completed process-log retention |
| `UAGENT_MCP_LOG_DAYS` / `UAGENT_MCP_LOG_FILES` | 7 / 100 | MCP log and captured-image retention |
| `UAGENT_TERMINAL_DAYS` | 7 | detached-terminal record retention; `0` removes completed records |
| `UAGENT_CONFIG_FILE` | empty | replace normal config-file lookup |
| `UAGENT_TRUST_PROJECT_CONFIG` | `0` | trust the current project config without prompting |

## MCP server configuration

Define user servers in `~/.mcp.json`. A project may define the same structure
in `./.mcp.json` after project configuration is trusted. Project entries win
name collisions, followed by user entries and then bundled servers.

```json
{
  "mcpServers": {
    "example": {
      "command": "example-mcp",
      "args": ["--stdio"],
      "env": {"EXAMPLE_TOKEN": "$EXAMPLE_TOKEN"},
      "roots": ["."],
      "tools": ["search"],
      "trust": false
    }
  }
}
```

| Field | Default | Purpose |
| --- | --- | --- |
| `command` | required | executable to start |
| `type` | `stdio` | transport; only `stdio` is supported |
| `args` | `[]` | command arguments |
| `env` | `{}` | child environment additions with process-variable expansion |
| `cwd` | inherited | server working directory; relative to the config file |
| `tools` | all | remote-tool name allowlist |
| `roots` | `UAGENT_MCP_ROOTS` or workspace | advertised filesystem roots; relative entries use the config directory |
| `trust` | `false` | allow read-only-annotated tools to run without mutation approval |
| `disabled` | `false` | keep the entry without starting it |

## Installation, build, and Action-only variables

These configure tooling rather than the installed runtime:

| Variable | Default | Purpose |
| --- | --- | --- |
| `UAGENT_BUILD_DIR` | `build-install` | `install.sh` build directory |
| `UAGENT_BUILD_JOBS` | 4 | parallel install build jobs |
| `UAGENT_PREFIX` | `~/.local` | install prefix |
| `UAGENT_SKILLS_DIR` | `~/.uagent/skills` | bundled-skill destination |
| `UAGENT_ACTION_PROMPT` | Action input | GitHub Action prompt |
| `UAGENT_ACTION_BUDGET` | Action input | GitHub Action budget |
| `UAGENT_ACTION_VERSION` | Action input | GitHub Action release version |

`UAGENT_WARNINGS_AS_ERRORS`, `UAGENT_ENABLE_ASAN`, `UAGENT_ENABLE_UBSAN`,
`UAGENT_ENABLE_TSAN`, `UAGENT_ENABLE_COVERAGE`, `UAGENT_BUILD_FUZZERS`, and
`UAGENT_BUILD_BENCHMARKS` are CMake cache options (`-D...=ON`), not runtime
environment settings.
