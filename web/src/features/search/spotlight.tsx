import "./spotlight.css";
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import type {
  Node,
  Session,
  SlashCommand,
  Snapshot,
} from "../../shared/types.ts";
import {
  Button,
  EmptyState,
  Input,
  Notice,
  useDialogClose,
} from "../../shared/ui.tsx";
import { folderName } from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import { useCompact } from "../../shared/layout.ts";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { SuggestionList } from "../../shared/suggestion-list.tsx";
import { fuzzy, matches } from "../../shared/fuzzy.ts";
import { occurrences } from "../../../sheet/text.js";
import { TOPICS } from "../../shared/settings-topics.ts";
import { findSettings, settingsEntries } from "../../shared/settings-index.ts";
import { SHORTCUTS, keysOf, type ShortcutId } from "../../shared/shortcuts.ts";
import { useResource } from "../../shared/use-resource.ts";
import { useApi } from "../../state/api.ts";

// A row that holds the words, as GET /api/search returns it.
type Hit = {
  session_id: string;
  title: string;
  sequence: number;
  snippet: string;
  rows: number;
};

type Row = {
  key: string;
  group: string;
  run: () => void;
} & (
  | { kind: "conversation"; label: string; detail: string }
  | { kind: "hit"; hit: Hit }
  | { kind: "more"; label: string }
  | {
      kind: "action";
      label: string;
      detail?: string;
      shortcut?: ShortcutId;
      about?: string;
    }
);

const RECENT = 8;
const NAMED = 6;
const HITS_SHOWN = 3;
const HITS_AFTER_MS = 200;
const HITS_FROM = 2;
const PREVIEW_AFTER_MS = 150;
// How much of a previewed row shows before its first match.
const PREVIEW_LEAD = 120;

function Keys({ id }: { id: ShortcutId }) {
  return (
    <span class="keys">
      {keysOf(id).map((key) => (
        <kbd class="small" key={key}>
          {key}
        </kbd>
      ))}
    </span>
  );
}

function Marked({ text, words }: { text: string; words: string }) {
  const parts = [];
  let from = 0;
  for (const at of occurrences(text, words)) {
    parts.push(text.slice(from, at));
    parts.push(<mark key={at}>{text.slice(at, at + words.length)}</mark>);
    from = at + words.length;
  }
  parts.push(text.slice(from));
  return <>{parts}</>;
}

function around(text: string, words: string) {
  const at = occurrences(text, words)[0] ?? 0;
  return at > PREVIEW_LEAD
    ? `…${text.slice(at - PREVIEW_LEAD).replace(/^\S*\s/, "")}`
    : text;
}

// ">" searches only actions, commands and settings. Groups keep their order
// while typing; the place of the open conversation comes first.
export default function Spotlight({
  sessions,
  commands,
  place,
  choose,
  find,
  start,
  run,
  settings,
  actions,
  conversation: inConversation,
}: {
  sessions: Session[];
  commands: SlashCommand[];
  place?: string;
  choose: (id: string) => void;
  find: (id: string, words: string, sequence: number) => void;
  start: (cwd: string) => void;
  run: (command: SlashCommand) => void;
  settings: (address: string) => void;
  actions: Partial<Record<ShortcutId, () => void>>;
  // Whether This conversation is a topic of Settings now.
  conversation: boolean;
}) {
  const { api, routes, manage } = useApi();
  const close = useDialogClose();
  const wide = !useCompact();
  const [query, setQuery] = useState("");
  const [index, setIndex] = useState(0);
  const root = useRef<HTMLDivElement>(null);
  const commandsOnly = query.trimStart().startsWith(">");
  const words = (commandsOnly ? query.trimStart().slice(1) : query).trim();
  const searching = !commandsOnly && words.length >= HITS_FROM;
  // The host reads every conversation, so it is asked once typing pauses,
  // and an answer to older words is dropped.
  const hits = useResource(
    (signal) =>
      searching
        ? api<{ hits: Hit[] }>(routes.search(words), undefined, {
            signal,
          }).then((value) => ({ words, items: value.hits }))
        : undefined,
    [words, searching, routes.search, api],
    undefined,
    HITS_AFTER_MS,
  );
  const items =
    searching && hits.value?.words === words ? hits.value.items : undefined;
  // Settings' own index, read once something is typed.
  const config = useResource(
    () => (words ? manage("config", { operation: "get" }) : undefined),
    [!!words, manage],
  );
  const byId = new Map(sessions.map((item) => [item.id, item]));
  const recent = [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const name = (item?: Session, title = "") =>
    item?.title || title || "New conversation";
  const conversation = (item: Session, group: string): Row => ({
    key: `session:${item.id}`,
    group,
    kind: "conversation",
    label: name(item),
    detail: folderName(folderOf(item)),
    run: () => choose(item.id),
  });
  const folders: Row[] = [...new Set(recent.map(folderOf).filter(Boolean))].map(
    (folder) => ({
      key: `folder:${folder}`,
      group: "Conversations",
      kind: "action" as const,
      label: `New conversation in ${folderName(folder)}`,
      detail: "Folder",
      run: () => start(folder),
    }),
  );
  const shortcuts: Row[] = SHORTCUTS.filter((item) => actions[item.id]).map(
    (item) => ({
      key: `action:${item.id}`,
      group: "Actions & commands",
      kind: "action",
      label: item.label,
      shortcut: item.id,
      run: actions[item.id]!,
    }),
  );
  const slashes: Row[] = commands
    .filter((entry) => entry.description && !entry.terminal)
    .map((entry) => ({
      key: `command:${entry.command}`,
      group: "Actions & commands",
      kind: "action",
      label: entry.command,
      about: entry.description,
      run: () => run(entry),
    }));
  const found: Row[] = findSettings(
    settingsEntries(
      config.value?.settings ?? [],
      config.value?.groups ?? [],
      TOPICS.map(({ id }) => id).filter(
        (id) => id !== "conversation" || inConversation,
      ),
    ),
    words,
  ).map((entry) => ({
    key: `settings:${entry.address}`,
    group: "Settings",
    kind: "action",
    label: entry.label,
    detail: entry.topic,
    run: () => settings(entry.address),
  }));
  const label = (row: Row) => ("label" in row ? row.label : "");
  // A name matches by its letters in order; what a command does only by
  // whole words, so a long description does not match everything.
  const named = (rows: Row[], text: string) => {
    const byName = fuzzy(rows, text, label);
    return [
      ...byName,
      ...rows.filter(
        (row) =>
          !byName.includes(row) && "about" in row && matches(text, row.about),
      ),
    ];
  };

  // One group per place, the open conversation's first, then as the host
  // ranked them; each conversation once, with its newest hits under it.
  const messages: Row[] = [];
  if (items) {
    const where = (hit: Hit) => {
      const item = byId.get(hit.session_id);
      return item ? folderOf(item) : "";
    };
    const places = [...new Set(items.map(where))].sort(
      (a, b) => Number(b === place) - Number(a === place),
    );
    for (const at of places) {
      const group = at ? folderName(at) : "Other conversations";
      const inPlace = items.filter((hit) => where(hit) === at);
      for (const id of new Set(inPlace.map((hit) => hit.session_id))) {
        const own = inPlace
          .filter((hit) => hit.session_id === id)
          .sort((a, b) => b.sequence - a.sequence);
        const item = byId.get(id);
        const total = own[0].rows;
        messages.push({
          key: `conversation:${id}`,
          group,
          kind: "conversation",
          label: name(item, own[0].title),
          detail: total === 1 ? "1 match" : `${total} matches`,
          run: () => choose(id),
        });
        for (const hit of own.slice(0, HITS_SHOWN)) {
          messages.push({
            key: `hit:${id}:${hit.sequence}`,
            group,
            kind: "hit",
            hit,
            run: () => find(id, words, hit.sequence),
          });
        }
        if (total > HITS_SHOWN) {
          messages.push({
            key: `more:${id}`,
            group,
            kind: "more",
            label: `${total - HITS_SHOWN} more in this conversation`,
            run: () => find(id, words, own[0].sequence),
          });
        }
      }
    }
  }

  const shown: Row[] = commandsOnly
    ? [...named([...shortcuts, ...slashes], words), ...found]
    : !words
      ? [
          ...recent
            .slice(0, RECENT)
            .map((item) => conversation(item, "Recent")),
          ...shortcuts,
        ]
      : [
          ...named(
            [
              ...recent.map((item) => conversation(item, "Conversations")),
              ...folders,
            ],
            words,
          ).slice(0, NAMED),
          ...messages,
          ...named([...shortcuts, ...slashes], words),
          ...found,
        ];
  const active = shown[Math.min(index, shown.length - 1)];
  useLayoutEffect(() => {
    root.current?.querySelector("input")?.focus({ preventScroll: true });
  }, []);
  const open = (row: Row) => {
    close();
    row.run();
  };
  return (
    <div class="spotlight" ref={root}>
      <Input
        type="search"
        aria-label="Search conversations, messages, commands and settings"
        placeholder="Search, or > for commands"
        role="combobox"
        aria-expanded={shown.length > 0}
        aria-controls="spotlight-results"
        aria-autocomplete="list"
        aria-activedescendant={active && `spotlight-${shown.indexOf(active)}`}
        value={query}
        onInput={(event) => {
          setQuery(event.currentTarget.value);
          setIndex(0);
        }}
        onKeyDown={(event) => {
          if (event.isComposing || !plainKey(event)) return;
          if (
            (event.key === "ArrowDown" || event.key === "ArrowUp") &&
            shown.length
          )
            setIndex(nextIndex(shown.indexOf(active), event.key, shown.length));
          else if (event.key === "Enter" && active) open(active);
          // Esc steps back: it clears the words, then closes.
          else if (event.key === "Escape" && query) {
            setQuery("");
            setIndex(0);
            event.stopPropagation();
          } else return;
          event.preventDefault();
        }}
      />
      <div class="spotlight-body">
        <SuggestionList
          id="spotlight-results"
          class="spotlight-results"
          label="Results"
          prefix="spotlight"
          items={shown}
          index={shown.indexOf(active)}
          pick={open}
          keyOf={(row) => row.key}
          groupOf={(row) => row.group}
        >
          {(row) =>
            row.kind === "hit" ? (
              <span class="spotlight-snippet small">
                <Marked text={row.hit.snippet} words={words} />
              </span>
            ) : row.kind === "more" ? (
              <span class="spotlight-more small">{row.label}</span>
            ) : (
              <>
                <span class="spotlight-label truncate">
                  {row.label}
                  {"about" in row && row.about && ` — ${row.about}`}
                </span>
                {row.detail && (
                  <span class="spotlight-detail small">{row.detail}</span>
                )}
                {row.kind === "action" && row.shortcut && (
                  <Keys id={row.shortcut} />
                )}
              </>
            )
          }
        </SuggestionList>
        {/* Kept while hits are listed, so the panel holds its width as the
            selection moves off one. */}
        {wide &&
          messages.length > 0 &&
          (active?.kind === "hit" ? (
            <Preview hit={active.hit} words={words} />
          ) : (
            <div class="spotlight-preview" />
          ))}
      </div>
      {searching && hits.error != null ? (
        <Notice
          tone="error"
          action={<Button onClick={hits.retry}>Retry</Button>}
        >
          Couldn’t search messages.
        </Notice>
      ) : searching && !items ? (
        <p class="small muted" role="status">
          Searching messages…
        </p>
      ) : (
        !shown.length && (
          <EmptyState>
            {commandsOnly ? "No command matches." : "Nothing matches."}
          </EmptyState>
        )
      )}
    </div>
  );
}

function Preview({ hit, words }: { hit: Hit; words: string }) {
  const { api, routes } = useApi();
  const cache = useRef(new Map<string, Node[]>());
  const id = `${hit.session_id}:${hit.sequence}`;
  const preview = useResource(
    async (signal) => {
      const held = cache.current.get(id);
      if (held) return { id, rows: held };
      const value = await api<Snapshot>(
        routes.session(hit.session_id, `before=${hit.sequence + 1}`),
        undefined,
        { signal },
      );
      const rows = (value.state?.view?.nodes || []).slice(-3);
      if (!signal.aborted) cache.current.set(id, rows);
      return { id, rows };
    },
    [id, routes.session, api],
    undefined,
    PREVIEW_AFTER_MS,
  );
  const rows = preview.value?.id === id ? preview.value.rows : undefined;
  const who = (node: Node) =>
    node.headline
      ? [node.headline.verb, node.headline.target].filter(Boolean).join(" ")
      : node.role === "assistant"
        ? "Agent"
        : "You";
  return (
    <aside class="spotlight-preview" aria-label="Preview">
      {preview.error != null ? (
        <Notice
          tone="error"
          action={<Button onClick={preview.retry}>Retry</Button>}
        >
          Preview unavailable.
        </Notice>
      ) : !rows ? (
        <p class="small">Loading…</p>
      ) : (
        rows.map((node) => (
          <p key={node.id} class="spotlight-preview-row">
            <strong>{who(node)}</strong>
            <Marked
              text={around(node.body?.text || "", words).slice(0, 600)}
              words={words}
            />
          </p>
        ))
      )}
    </aside>
  );
}

export function ShortcutList() {
  return (
    <dl class="shortcut-list">
      {SHORTCUTS.map((item) => (
        <div key={item.id}>
          <dt>
            <Keys id={item.id} />
          </dt>
          <dd>{item.label}</dd>
        </div>
      ))}
    </dl>
  );
}
