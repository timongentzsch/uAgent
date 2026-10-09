import { useEffect, useRef } from "preact/hooks";

// Every keyboard shortcut, once: the global listener below matches these,
// and the shortcuts sheet and command palette show them. `keys` is
// "Mod+K" style, Mod being ⌘ on a Mac and Ctrl elsewhere. Entries without
// `global` are the composer's own, listed so they can be found.
export const SHORTCUTS = [
  { id: "palette", keys: "Mod+K", label: "Command palette", global: true },
  { id: "shortcuts", keys: "?", label: "Keyboard shortcuts", global: true },
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
};

// A shortcut's keys as they are printed on the keyboard.
export const keysOf = (id: ShortcutId) =>
  SHORTCUTS.find((item) => item.id === id)!
    .keys.split("+")
    .map((key) => NAMES[key] || key);

// Someone typing: `?` is text there, not a shortcut.
const typing = (target: EventTarget | null) =>
  target instanceof HTMLElement &&
  (target.isContentEditable ||
    target.matches("input, textarea, select, [role=textbox]"));

function matches(keys: string, event: KeyboardEvent) {
  const parts = keys.split("+");
  const key = parts.pop()!;
  const mod = parts.includes("Mod");
  // `?` needs Shift on most layouts, so a bare key ignores Shift.
  return (
    event.key.toLowerCase() === key.toLowerCase() &&
    (mac ? event.metaKey : event.ctrlKey) === mod &&
    event.altKey === parts.includes("Alt") &&
    (key.length === 1 || event.shiftKey === parts.includes("Shift"))
  );
}

// One keydown listener for the whole app runs the global shortcuts given
// handlers; the latest handlers run, without re-subscribing.
export function useShortcuts(
  handlers: Partial<Record<ShortcutId, () => void>>,
) {
  const latest = useRef(handlers);
  latest.current = handlers;
  useEffect(() => {
    const keyDown = (event: KeyboardEvent) => {
      if (event.defaultPrevented || event.isComposing) return;
      const shortcut = SHORTCUTS.find(
        (item) =>
          "global" in item &&
          latest.current[item.id] &&
          matches(item.keys, event) &&
          !(item.keys === "?" && typing(event.target)),
      );
      if (!shortcut) return;
      event.preventDefault();
      latest.current[shortcut.id]!();
    };
    addEventListener("keydown", keyDown);
    return () => removeEventListener("keydown", keyDown);
  }, []);
}
