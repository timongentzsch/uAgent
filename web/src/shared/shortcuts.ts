import { useEffect } from "preact/hooks";
import { useLatest } from "./use-latest.ts";

// Every keyboard shortcut, once: the global listener below matches these,
// and the shortcuts sheet and search show them. `keys` is
// "Mod+K" style, Mod being ⌘ on a Mac and Ctrl elsewhere; Ctrl is Ctrl
// everywhere, and ⌘ too on a Mac. Entries without `global` are the
// composer's own, listed so they can be found. `outside` ones are text in
// a field, and a dialog's or menu's own keys there: they act only outside.
export const SHORTCUTS = [
  { id: "search", keys: "Mod+K", label: "Search", global: true },
  {
    id: "find",
    keys: "Mod+F",
    label: "Find in this conversation",
    global: true,
  },
  {
    id: "shortcuts",
    keys: "?",
    label: "Keyboard shortcuts",
    global: true,
    outside: true,
  },
  {
    id: "previous",
    keys: "Alt+ArrowUp",
    label: "Previous conversation",
    global: true,
  },
  {
    id: "next",
    keys: "Alt+ArrowDown",
    label: "Next conversation",
    global: true,
  },
  {
    id: "terminal",
    keys: "Ctrl+`",
    label: "Show or hide the terminal",
    global: true,
  },
  {
    id: "select-all",
    keys: "Mod+A",
    label: "Select all conversations, while selecting",
    global: true,
    outside: true,
  },
  {
    id: "delete-selected",
    keys: "Backspace",
    label: "Delete the selected conversations",
    global: true,
    outside: true,
  },
  {
    id: "deselect",
    keys: "Escape",
    label: "Stop selecting",
    global: true,
    outside: true,
  },
  { id: "send", keys: "Enter", label: "Send the message" },
  { id: "newline", keys: "Shift+Enter", label: "New line in the message" },
  { id: "queue", keys: "Alt+Enter", label: "Send when the running turn ends" },
  { id: "stop", keys: "Escape", label: "Stop the running turn" },
  { id: "recall", keys: "ArrowUp", label: "Recall a message you sent" },
  { id: "commands", keys: "/", label: "Slash commands, in the composer" },
  { id: "complete", keys: "Tab", label: "Complete a command" },
  { id: "mention", keys: "@", label: "Mention an attached file" },
  { id: "close", keys: "Escape", label: "Close a dialog or menu" },
] as const;
export type ShortcutId = (typeof SHORTCUTS)[number]["id"];

const mac =
  typeof navigator !== "undefined" &&
  /Mac|iPhone|iPad/.test(navigator.platform);
const NAMES: Record<string, string> = {
  Mod: mac ? "⌘" : "Ctrl",
  Alt: mac ? "⌥" : "Alt",
  Shift: "Shift",
  ArrowUp: "↑",
  ArrowDown: "↓",
  Escape: "Esc",
  Backspace: "⌫",
};

// A shortcut's keys as they are printed on the keyboard.
export const keysOf = (id: ShortcutId) =>
  SHORTCUTS.find((item) => item.id === id)!
    .keys.split("+")
    .map((key) => NAMES[key] || key);

// Someone typing: `?` is text there, not a shortcut.
export const isTextEntry = (target: EventTarget | null) =>
  target instanceof HTMLElement &&
  (target.isContentEditable ||
    target.matches(
      "input:not([type=checkbox], [type=radio]), textarea, select, [role=textbox]",
    ));

function matches(keys: string, event: KeyboardEvent) {
  const parts = keys.split("+");
  const key = parts.pop()!;
  const mod = parts.includes("Mod");
  const modifier = parts.includes("Ctrl")
    ? event.ctrlKey || (mac && event.metaKey)
    : (mac ? event.metaKey : event.ctrlKey) === mod;
  // `?` needs Shift on most layouts, so a bare key ignores Shift.
  return (
    event.key.toLowerCase() === key.toLowerCase() &&
    modifier &&
    event.altKey === parts.includes("Alt") &&
    (key.length === 1 || event.shiftKey === parts.includes("Shift"))
  );
}

// A keydown listener runs the global shortcuts given handlers (the app's,
// and the sidebar's while it selects); the latest handlers run, without
// re-subscribing.
export function useShortcuts(
  handlers: Partial<Record<ShortcutId, () => void>>,
) {
  const latest = useLatest(handlers);
  useEffect(() => {
    const keyDown = (event: KeyboardEvent) => {
      if (event.defaultPrevented || event.isComposing) return;
      const shortcut = SHORTCUTS.find(
        (item) =>
          "global" in item &&
          latest.current[item.id] &&
          matches(item.keys, event) &&
          !(
            "outside" in item &&
            (isTextEntry(event.target) ||
              (event.target as Element).closest?.("dialog, [role=menu]"))
          ),
      );
      if (!shortcut) return;
      event.preventDefault();
      latest.current[shortcut.id]!();
    };
    addEventListener("keydown", keyDown);
    return () => removeEventListener("keydown", keyDown);
  }, []);
}
