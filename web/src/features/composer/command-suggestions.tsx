import { useState } from "preact/hooks";
import type { RefObject, TargetedKeyboardEvent } from "preact";
import type { SlashCommand } from "../../shared/types.ts";
import { slashCompletion, slashMatches } from "./slash.ts";
import { SuggestionList } from "../../shared/suggestion-list.tsx";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";

// What picking a command puts in the composer: a space follows when it
// takes an argument.
const completionOf = (entry: SlashCommand) =>
  entry.command + (entry.argument ? " " : "");

export function useCommandSuggestions(
  commands: SlashCommand[],
  text: string,
  change: (text: string) => void,
  input: RefObject<HTMLTextAreaElement | null>,
) {
  const [selection, select] = useState({ text: "", index: -1 });
  const [dismissed, dismiss] = useState<string | null>(null);
  const [focused, focus] = useState(false);
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
  function keyDown(event: TargetedKeyboardEvent<HTMLTextAreaElement>) {
    if (!matches.length || event.keyCode === 229 || !plainKey(event))
      return false;
    if (event.key === "Escape") dismiss(text);
    else if (event.key === "ArrowDown" || event.key === "ArrowUp")
      select({ text, index: nextIndex(index, event.key, matches.length) });
    else if (event.key === "Tab") {
      const value = active ? completionOf(active) : slashCompletion(matches);
      if (value !== text) change(value);
      if (active || matches.length === 1) dismiss(value);
    } else if (event.key === "Enter" && active) {
      if (!event.repeat) complete(completionOf(active));
    } else return false;
    event.preventDefault();
    return true;
  }
  return {
    keyDown,
    count: matches.length,
    // The textarea is the combobox: it keeps focus, the list pops up.
    attributes: {
      role: "combobox" as const,
      "aria-haspopup": "listbox" as const,
      "aria-expanded": matches.length > 0,
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
      <SuggestionList
        id="command-suggestions"
        label="Slash commands"
        prefix="command"
        items={matches}
        index={index}
        pick={(entry) => complete(completionOf(entry))}
      >
        {(entry) => (
          <>
            <strong>{entry.command}</strong>
            <span>{entry.description}</span>
          </>
        )}
      </SuggestionList>
    ),
  };
}
