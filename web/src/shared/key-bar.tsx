import type { ComponentChildren } from "preact";
import "./key-bar.css";
import { Button } from "./ui.tsx";

// The keys a touch keyboard lacks, by their KeyboardEvent.key name, and a
// sticky Ctrl for the next key typed. The browser viewer and the terminal
// each send a key their own way.
export const BAR_KEYS = {
  Escape: "Esc",
  Tab: "Tab",
  ArrowLeft: "←",
  ArrowUp: "↑",
  ArrowDown: "↓",
  ArrowRight: "→",
} as const;
export type BarKey = keyof typeof BAR_KEYS;
const NAMES = Object.keys(BAR_KEYS) as BarKey[];

// A press leaves the focus where it is, so the keyboard stays open.
export const keepFocus = (event: Event) => event.preventDefault();

export function KeyBar({
  press,
  ctrl,
  setCtrl,
  children,
}: {
  press: (key: BarKey) => void;
  ctrl: boolean;
  setCtrl: (ctrl: boolean) => void;
  // More keys at the end, which share the width.
  children?: ComponentChildren;
}) {
  return (
    <div class="key-bar" aria-label="Special keys">
      {NAMES.map((name) => (
        <Button
          key={name}
          aria-label={name.replace("Arrow", "Arrow ")}
          onPointerDown={keepFocus}
          onClick={() => press(name)}
        >
          {BAR_KEYS[name]}
        </Button>
      ))}
      <Button
        aria-pressed={ctrl}
        onPointerDown={keepFocus}
        onClick={() => setCtrl(!ctrl)}
      >
        Ctrl
      </Button>
      {children}
    </div>
  );
}
