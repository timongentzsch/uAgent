import "./spotlight.css";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import type {
  Block,
  Session,
  SlashCommand,
  Snapshot,
} from "../../shared/types.ts";
import { Input, useDialogClose } from "../../shared/ui.tsx";
import { folderName } from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import { useCompact } from "../../shared/layout.ts";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { SuggestionList } from "../../shared/suggestion-list.tsx";
import { fuzzy } from "../../shared/fuzzy.ts";
import { occurrences } from "../../shared/find.ts";
import { SCOPES, type Scope } from "../../shared/settings-scopes.ts";
import { SHORTCUTS, keysOf, type ShortcutId } from "../../shared/shortcuts.ts";
import { api, routes } from "../../state/api.ts";

// A row of some conversation that holds the words (GET /api/search): its
// conversation, where, and how many of that conversation's rows hold them.
type Hit = {
  session_id: string;
  title: string;
  sequence: number;
  snippet: string;
  rows: number;
};

// One line of results, under the group it belongs to.
type Row = {
  key: string;
  group: string;
  run: () => void;
} & (
  | { kind: "conversation"; label: string; detail: string }
  | { kind: "hit"; hit: Hit }
  | { kind: "more"; label: string }
  | { kind: "action"; label: string; detail: string; shortcut?: ShortcutId }
);

const RECENT = 8;
const NAMED = 6;
const HITS_SHOWN = 3;
const HITS_AFTER_MS = 200;
const HITS_FROM = 2;
const PREVIEW_AFTER_MS = 150;
// How much of a previewed row shows before its first match.
const PREVIEW_LEAD = 120;

// The keys of one shortcut, as printed on the keyboard.
export function Keys({ id }: { id: ShortcutId }) {
  return (
    <span class="keys">
      {keysOf(id).map((key) => (
        <kbd key={key}>{key}</kbd>
      ))}
    </span>
  );
}

// Text with each occurrence of the words marked.
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

// The part of a text from shortly before its first match, on a word.
function around(text: string, words: string) {
  const at = occurrences(text, words)[0] ?? 0;
  return at > PREVIEW_LEAD
    ? `…${text.slice(at - PREVIEW_LEAD).replace(/^\S*\s/, "")}`
    : text;
}

// Search: conversations by name, then the messages of every conversation
// that hold the words, grouped by place and conversation, then the app's
// actions, commands and settings. ">" searches only those. Groups keep their
// order while typing; the place of the open conversation comes first.
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
}: {
  sessions: Session[];
  commands: SlashCommand[];
  // The folder or workspace of the open conversation, whose hits lead.
  place?: string;
  choose: (id: string) => void;
  // Opens a conversation at a row, with its find bar on the words.
  find: (id: string, words: string, sequence: number) => void;
  // A new conversation in a folder, or the folder's lead agent.
  start: (cwd: string, panel?: boolean) => void;
  // A slash command, as if typed in the composer.
  run: (command: SlashCommand) => void;
  settings: (scope: Scope) => void;
  // The global shortcuts' actions, listed by their names.
  actions: Partial<Record<ShortcutId, () => void>>;
}) {
  const close = useDialogClose();
  const wide = !useCompact();
  const [query, setQuery] = useState("");
  const [hits, setHits] = useState<{ words: string; items: Hit[] }>({
    words: "",
    items: [],
  });
  const [index, setIndex] = useState(0);
  const root = useRef<HTMLDivElement>(null);
  const commandsOnly = query.trimStart().startsWith(">");
  const words = (commandsOnly ? query.trimStart().slice(1) : query).trim();
  const searching = !commandsOnly && words.length >= HITS_FROM;
  // The host reads every conversation, so it is asked once typing pauses,
  // and an answer to older words is dropped.
  useEffect(() => {
    if (!searching) return;
    const controller = new AbortController();
    const timer = setTimeout(() => {
      api<{ hits: Hit[] }>(routes.search(words), undefined, {
        signal: controller.signal,
      })
        .then((value) => setHits({ words, items: value.hits }))
        .catch(() => {});
    }, HITS_AFTER_MS);
    return () => {
      clearTimeout(timer);
      controller.abort();
    };
  }, [words, searching]);

  const byId = new Map(sessions.map((item) => [item.id, item]));
  const recent = [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const name = (item?: Session, title = "") =>
    item?.kind === "panel"
      ? `Lead · ${folderName(item.cwd)}`
      : item?.title || title || "New conversation";
  const conversation = (item: Session, group: string): Row => ({
    key: `session:${item.id}`,
    group,
    kind: "conversation",
    label: name(item),
    detail: folderName(folderOf(item)),
    run: () => choose(item.id),
  });
  const folders: Row[] = [
    ...new Set(recent.map(folderOf).filter(Boolean)),
  ].flatMap((folder) => [
    {
      key: `folder:${folder}`,
      group: "Conversations",
      kind: "action" as const,
      label: `New conversation in ${folderName(folder)}`,
      detail: "Folder",
      run: () => start(folder),
    },
    // One that exists is listed among the conversations.
    ...(recent.some(
      (item) => item.kind === "panel" && folderOf(item) === folder,
    )
      ? []
      : [
          {
            key: `panel:${folder}`,
            group: "Conversations",
            kind: "action" as const,
            label: `Lead · ${folderName(folder)}`,
            detail: "Folder",
            run: () => start(folder, true),
          },
        ]),
  ]);
  const shortcuts: Row[] = SHORTCUTS.filter((item) => actions[item.id]).map(
    (item) => ({
      key: `action:${item.id}`,
      group: "Actions & commands",
      kind: "action",
      label: item.label,
      detail: "Action",
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
      label: `${entry.command} — ${entry.description}`,
      detail: "Command",
      run: () => run(entry),
    }));
  const scopes: Row[] = SCOPES.map(([id, label]) => ({
    key: `settings:${id}`,
    group: "Settings",
    kind: "action",
    label: `Settings: ${label}`,
    detail: "Settings",
    run: () => settings(id),
  }));
  const label = (row: Row) => ("label" in row ? row.label : "");
  const named = (rows: Row[], text: string) => fuzzy(rows, text, label);

  // Message hits: one group per place, the open conversation's first and
  // then as the host ranked them; in each, a conversation once, its newest
  // hits under it, and how many more it holds.
  const messages: Row[] = [];
  if (searching && hits.words === words) {
    const where = (hit: Hit) => {
      const item = byId.get(hit.session_id);
      return item ? folderOf(item) : "";
    };
    const places = [...new Set(hits.items.map(where))].sort(
      (a, b) => Number(b === place) - Number(a === place),
    );
    for (const at of places) {
      const group = at ? folderName(at) : "Other conversations";
      const inPlace = hits.items.filter((hit) => where(hit) === at);
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
    ? [...named([...shortcuts, ...slashes], words), ...named(scopes, words)]
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
          ...named(scopes, words),
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
              <span class="spotlight-snippet">
                <Marked text={row.hit.snippet} words={words} />
              </span>
            ) : row.kind === "more" ? (
              <span class="spotlight-more">{row.label}</span>
            ) : (
              <>
                <span class="spotlight-label">{row.label}</span>
                <span class="spotlight-detail">{row.detail}</span>
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
      {!shown.length && (
        <p class="spotlight-empty">
          {commandsOnly ? "No command matches." : "Nothing matches."}
        </p>
      )}
    </div>
  );
}

// The rows around a hit, read once the selection rests on it: what was said
// before it and the row itself, with the words marked.
function Preview({ hit, words }: { hit: Hit; words: string }) {
  const [rows, setRows] = useState<Block[] | null>(null);
  const cache = useRef(new Map<string, Block[]>());
  const id = `${hit.session_id}:${hit.sequence}`;
  useEffect(() => {
    const held = cache.current.get(id);
    setRows(held ?? null);
    if (held) return;
    const controller = new AbortController();
    const timer = setTimeout(() => {
      api<Snapshot>(
        routes.session(hit.session_id, `before=${hit.sequence + 1}`),
        undefined,
        { signal: controller.signal },
      )
        .then((value) => {
          const blocks = (value.state?.view?.blocks || []).slice(-3);
          cache.current.set(id, blocks);
          setRows(blocks);
        })
        .catch(() => {});
    }, PREVIEW_AFTER_MS);
    return () => {
      clearTimeout(timer);
      controller.abort();
    };
  }, [id, hit.session_id, hit.sequence]);
  const who = (block: Block) =>
    block.kind === "user"
      ? "You"
      : block.kind === "assistant"
        ? "Agent"
        : block.kind === "tool_result"
          ? block.name || "Tool"
          : block.kind;
  return (
    <aside class="spotlight-preview" aria-label="Preview">
      {rows === null ? (
        <p class="spotlight-empty">Loading…</p>
      ) : (
        rows.map((block) => (
          <p key={block.id} class="spotlight-preview-row">
            <strong>{who(block)}</strong>
            <Marked
              text={around(block.text || "", words).slice(0, 600)}
              words={words}
            />
          </p>
        ))
      )}
    </aside>
  );
}

// The `?` sheet: every shortcut, with its keys.
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
