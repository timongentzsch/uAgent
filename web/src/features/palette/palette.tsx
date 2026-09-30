import "./palette.css";
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import type { Session, SlashCommand } from "../../shared/types.ts";
import { Button, Input, useDialogClose } from "../../shared/ui.tsx";
import { folderName, folderOf } from "../../shared/folder-label.tsx";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { fuzzy } from "../composer/slash.ts";
import { SECTIONS, type Section } from "../settings/settings-nav.tsx";
import { SHORTCUTS, keysOf, type ShortcutId } from "../../shared/shortcuts.ts";

type Item = {
  key: string;
  label: string;
  // What kind of thing it is, or where.
  detail: string;
  shortcut?: ShortcutId;
  run: () => void;
};

const SHOWN = 50;

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

// Everything reachable by name: conversations, folders, slash commands,
// settings sections and the app's own actions. Letters typed in order find
// an item (a prefix first); arrows move, Enter opens.
export default function Palette({
  sessions,
  commands,
  choose,
  start,
  run,
  settings,
  actions,
}: {
  sessions: Session[];
  commands: SlashCommand[];
  choose: (id: string) => void;
  // A new conversation in a folder.
  start: (cwd: string) => void;
  // A slash command, as if typed in the composer.
  run: (command: SlashCommand) => void;
  settings: (section: Section) => void;
  // The global shortcuts' actions, listed by their names.
  actions: Partial<Record<ShortcutId, () => void>>;
}) {
  const close = useDialogClose();
  const [query, setQuery] = useState("");
  const [index, setIndex] = useState(0);
  const root = useRef<HTMLDivElement>(null);
  const list = useRef<HTMLDivElement>(null);
  const recent = [...sessions].sort(
    (a, b) => (b.updated || 0) - (a.updated || 0),
  );
  const items: Item[] = [
    ...SHORTCUTS.filter((item) => actions[item.id]).map((item) => ({
      key: `action:${item.id}`,
      label: item.label,
      detail: "Action",
      shortcut: item.id,
      run: actions[item.id]!,
    })),
    ...recent.map((item) => ({
      key: `session:${item.id}`,
      label:
        item.kind === "coordinator"
          ? `Coordinator · ${folderName(item.cwd)}`
          : item.title || "New conversation",
      detail: folderName(folderOf(item)),
      run: () => choose(item.id),
    })),
    ...[...new Set(recent.map(folderOf).filter(Boolean))].map((folder) => ({
      key: `folder:${folder}`,
      label: `New conversation in ${folderName(folder)}`,
      detail: "Folder",
      run: () => start(folder),
    })),
    ...commands
      .filter((entry) => entry.description)
      .map((entry) => ({
        key: `command:${entry.command}`,
        label: `${entry.command} — ${entry.description}`,
        detail: "Command",
        run: () => run(entry),
      })),
    ...SECTIONS.map(([id, label]) => ({
      key: `settings:${id}`,
      label: `Settings: ${label}`,
      detail: "Settings",
      run: () => settings(id),
    })),
  ];
  const shown = fuzzy(items, query, (item) => item.label).slice(0, SHOWN);
  const active = shown[Math.min(index, shown.length - 1)];
  useLayoutEffect(() => {
    root.current?.querySelector("input")?.focus({ preventScroll: true });
  }, []);
  useLayoutEffect(() => {
    list.current
      ?.querySelector('[aria-selected="true"]')
      ?.scrollIntoView({ block: "nearest" });
  }, [active]);
  const open = (item: Item) => {
    close();
    item.run();
  };
  return (
    <div class="palette" ref={root}>
      <Input
        type="search"
        aria-label="Find a conversation, command or setting"
        placeholder="Type to search…"
        role="combobox"
        aria-expanded={shown.length > 0}
        aria-controls="palette-results"
        aria-autocomplete="list"
        aria-activedescendant={active && `palette-${shown.indexOf(active)}`}
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
      <div
        id="palette-results"
        class="palette-results"
        role="listbox"
        aria-label="Results"
        ref={list}
      >
        {shown.map((item, position) => (
          <Button
            key={item.key}
            variant="quiet"
            role="option"
            id={`palette-${position}`}
            aria-selected={item === active}
            tabIndex={-1}
            onMouseDown={(event) => event.preventDefault()}
            onClick={() => open(item)}
          >
            <span class="palette-label">{item.label}</span>
            <span class="palette-detail">{item.detail}</span>
            {item.shortcut && <Keys id={item.shortcut} />}
          </Button>
        ))}
      </div>
      {!shown.length && <p class="palette-empty">Nothing matches.</p>}
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
