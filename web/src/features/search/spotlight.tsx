import "./spotlight.css";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import type { Session, SlashCommand } from "../../shared/types.ts";
import { Input, useDialogClose } from "../../shared/ui.tsx";
import { folderName } from "../../shared/folder-label.tsx";
import { folderOf } from "../../shared/display.ts";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { SuggestionList } from "../../shared/suggestion-list.tsx";
import { fuzzy } from "../../shared/fuzzy.ts";
import { occurrences } from "../../shared/find.ts";
import { SCOPES, type Scope } from "../../shared/settings-scopes.ts";
import { SHORTCUTS, keysOf, type ShortcutId } from "../../shared/shortcuts.ts";
import { api, routes } from "../../state/api.ts";

type Item = {
  key: string;
  label: string;
  // What kind of thing it is, or where.
  detail: string;
  // A message hit's words around the match.
  snippet?: string;
  shortcut?: ShortcutId;
  run: () => void;
};

// A row of some conversation that holds the words (GET /api/search).
type Hit = {
  session_id: string;
  title: string;
  sequence: number;
  snippet: string;
};

const SHOWN = 50;
const HITS_AFTER_MS = 200;
const HITS_FROM = 2;

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

// Search: conversations by name, then the messages of every conversation
// that hold the words, then the app's actions, commands and settings.
// Letters typed in order find a name (a prefix first); arrows move, Enter
// opens.
export default function Spotlight({
  sessions,
  commands,
  choose,
  find,
  start,
  run,
  settings,
  actions,
}: {
  sessions: Session[];
  commands: SlashCommand[];
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
  const [query, setQuery] = useState("");
  const [hits, setHits] = useState<{ words: string; items: Hit[] }>({
    words: "",
    items: [],
  });
  const [index, setIndex] = useState(0);
  const root = useRef<HTMLDivElement>(null);
  const words = query.trim();
  // The host reads every conversation, so it is asked once typing pauses,
  // and an answer to older words is dropped.
  useEffect(() => {
    if (words.length < HITS_FROM) return;
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
  }, [words]);
  const recent = [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const byId = new Map(sessions.map((item) => [item.id, item]));
  const name = (item?: Session, title = "") =>
    item?.kind === "panel"
      ? `Lead · ${folderName(item.cwd)}`
      : item?.title || title || "New conversation";
  const places: Item[] = [
    ...recent.map((item) => ({
      key: `session:${item.id}`,
      label: name(item),
      detail: folderName(folderOf(item)),
      run: () => choose(item.id),
    })),
    ...[...new Set(recent.map(folderOf).filter(Boolean))].flatMap((folder) => [
      {
        key: `folder:${folder}`,
        label: `New conversation in ${folderName(folder)}`,
        detail: "Folder",
        run: () => start(folder),
      },
      // One that exists is listed among the conversations above.
      ...(recent.some(
        (item) => item.kind === "panel" && folderOf(item) === folder,
      )
        ? []
        : [
            {
              key: `panel:${folder}`,
              label: `Lead · ${folderName(folder)}`,
              detail: "Folder",
              run: () => start(folder, true),
            },
          ]),
    ]),
  ];
  const messages: Item[] =
    words.length >= HITS_FROM && hits.words === words
      ? hits.items.map((hit) => {
          const item = byId.get(hit.session_id);
          return {
            key: `hit:${hit.session_id}:${hit.sequence}`,
            label: name(item, hit.title),
            detail: item ? folderName(folderOf(item)) : "Message",
            snippet: hit.snippet,
            run: () => find(hit.session_id, words, hit.sequence),
          };
        })
      : [];
  const shortcuts: Item[] = SHORTCUTS.filter((item) => actions[item.id]).map(
    (item) => ({
      key: `action:${item.id}`,
      label: item.label,
      detail: "Action",
      shortcut: item.id,
      run: actions[item.id]!,
    }),
  );
  const rest: Item[] = [
    ...commands
      .filter((entry) => entry.description && !entry.terminal)
      .map((entry) => ({
        key: `command:${entry.command}`,
        label: `${entry.command} — ${entry.description}`,
        detail: "Command",
        run: () => run(entry),
      })),
    ...SCOPES.map(([id, label]) => ({
      key: `settings:${id}`,
      label: `Settings: ${label}`,
      detail: "Settings",
      run: () => settings(id),
    })),
  ];
  const named = (items: Item[]) => fuzzy(items, query, (item) => item.label);
  const shown = (
    words
      ? [...named(places), ...messages, ...named([...shortcuts, ...rest])]
      : [...shortcuts, ...places, ...rest]
  ).slice(0, SHOWN);
  const active = shown[Math.min(index, shown.length - 1)];
  useLayoutEffect(() => {
    root.current?.querySelector("input")?.focus({ preventScroll: true });
  }, []);
  const open = (item: Item) => {
    close();
    item.run();
  };
  return (
    <div class="spotlight" ref={root}>
      <Input
        type="search"
        aria-label="Search conversations, messages, commands and settings"
        placeholder="Search…"
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
          else return;
          event.preventDefault();
        }}
      />
      <SuggestionList
        id="spotlight-results"
        class="spotlight-results"
        label="Results"
        prefix="spotlight"
        items={shown}
        index={shown.indexOf(active)}
        pick={open}
        keyOf={(item) => item.key}
      >
        {(item) =>
          item.snippet ? (
            <span class="spotlight-hit">
              <span class="spotlight-line">
                <span class="spotlight-label">{item.label}</span>
                <span class="spotlight-detail">{item.detail}</span>
              </span>
              <span class="spotlight-snippet">
                <Marked text={item.snippet} words={words} />
              </span>
            </span>
          ) : (
            <>
              <span class="spotlight-label">{item.label}</span>
              <span class="spotlight-detail">{item.detail}</span>
              {item.shortcut && <Keys id={item.shortcut} />}
            </>
          )
        }
      </SuggestionList>
      {!shown.length && <p class="spotlight-empty">Nothing matches.</p>}
    </div>
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
