import "./command-suggestions.css";
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import type { JSX, RefObject } from "preact";
import type { SlashCommand } from "./types.ts";
import { slashCompletion, slashMatches } from "./slash.ts";

export function useCommandSuggestions(
  commands: SlashCommand[],
  text: string,
  change: (text: string) => void,
  input: RefObject<HTMLTextAreaElement>,
) {
  const [selection, select] = useState({ text: "", index: -1 });
  const [dismissed, dismiss] = useState<string | null>(null);
  const [focused, focus] = useState(false);
  const list = useRef<HTMLDivElement>(null);
  const matches =
    focused && dismissed !== text ? slashMatches(commands, text) : [];
  const index = selection.text === text ? selection.index : -1;
  const active = matches[index];
  function complete(value: string) {
    change(value);
    dismiss(value);
    select({ text: value, index: -1 });
    input.current?.focus({ preventScroll: true });
  }
  useLayoutEffect(() => {
    list.current
      ?.querySelector('[aria-selected="true"]')
      ?.scrollIntoView({ block: "nearest" });
  }, [index]);
  function keyDown(event: JSX.TargetedKeyboardEvent<HTMLTextAreaElement>) {
    if (
      !matches.length ||
      event.isComposing ||
      event.keyCode === 229 ||
      event.shiftKey ||
      event.ctrlKey ||
      event.metaKey ||
      event.altKey
    )
      return false;
    if (event.key === "Escape") dismiss(text);
    else if (event.key === "ArrowDown" || event.key === "ArrowUp")
      select({
        text,
        index:
          (index +
            (event.key === "ArrowDown" ? 1 : index < 0 ? 0 : -1) +
            matches.length) %
          matches.length,
      });
    else if (event.key === "Tab") {
      const value = active
        ? active.command + (active.argument ? " " : "")
        : slashCompletion(matches);
      if (value !== text) change(value);
      if (active || matches.length === 1) dismiss(value);
    } else if (event.key === "Enter" && active) {
      if (!event.repeat)
        complete(active.command + (active.argument ? " " : ""));
    } else return false;
    event.preventDefault();
    return true;
  }
  return {
    keyDown,
    attributes: {
      "aria-autocomplete": "list" as const,
      "aria-controls": matches.length ? "command-suggestions" : undefined,
      "aria-activedescendant": active ? `command-${index}` : undefined,
      onFocus: () => {
        focus(true);
        dismiss(null);
      },
      onBlur: () => focus(false),
    },
    list: matches.length > 0 && (
      <div
        id="command-suggestions"
        class="command-suggestions"
        role="listbox"
        aria-label="Slash commands"
        ref={list}
      >
        {matches.map((entry, position) => (
          <button
            type="button"
            role="option"
            id={`command-${position}`}
            aria-selected={position === index}
            tabIndex={-1}
            onMouseDown={(event) => event.preventDefault()}
            onClick={() =>
              complete(entry.command + (entry.argument ? " " : ""))
            }
          >
            <strong>{entry.command}</strong>
            <span>{entry.description}</span>
          </button>
        ))}
      </div>
    ),
  };
}
