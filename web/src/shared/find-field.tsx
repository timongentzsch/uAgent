import { ChevronDown, ChevronUp, X } from "lucide-preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Button, IconButton, Input, InputRow } from "./ui.tsx";

// What is searched, and how an occurrence is shown, is the caller's; so is
// replacing the current occurrence, or all of them, where it is offered.
export function FindField({
  label,
  words,
  setWords,
  count,
  current,
  step,
  replace,
  close,
}: {
  label: string;
  words: string;
  setWords: (words: string) => void;
  // How many occurrences there are; undefined while they are not counted.
  count?: number;
  // The current one, from 0.
  current: number;
  step: (by: number) => void;
  replace?: (by: string, all: boolean) => void;
  close: () => void;
}) {
  const bar = useRef<HTMLElement>(null);
  const [by, setBy] = useState("");
  // Focused as it opens. A frame later: a menu that opened the bar gives
  // focus back to its button as it closes.
  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      const field = bar.current?.querySelector("input");
      field?.focus({ preventScroll: true });
      field?.select();
    });
    return () => cancelAnimationFrame(frame);
  }, []);
  return (
    <InputRow class="find-bar" role="search" aria-label={label} rowRef={bar}>
      <Input
        type="search"
        aria-label={label}
        placeholder={`${label}…`}
        value={words}
        onInput={(event) => setWords(event.currentTarget.value)}
        onKeyDown={(event) => {
          if (event.isComposing) return;
          if (event.key === "Enter") step(event.shiftKey ? -1 : 1);
          else if (event.key === "Escape") close();
          else return;
          event.preventDefault();
          event.stopPropagation();
        }}
      />
      <span class="find-count small" aria-live="polite">
        {count ? `${current + 1} of ${count}` : count === 0 ? "No matches" : ""}
      </span>
      <IconButton
        label="Previous match"
        disabled={!count}
        onClick={() => step(-1)}
      >
        <ChevronUp />
      </IconButton>
      <IconButton label="Next match" disabled={!count} onClick={() => step(1)}>
        <ChevronDown />
      </IconButton>
      {replace && (
        <>
          <Input
            type="text"
            aria-label="Replace with"
            placeholder="Replace with…"
            value={by}
            onInput={(event) => setBy(event.currentTarget.value)}
            onKeyDown={(event) => {
              if (event.isComposing || event.key !== "Enter" || !count) return;
              event.preventDefault();
              replace(by, false);
            }}
          />
          <Button disabled={!count} onClick={() => replace(by, false)}>
            Replace
          </Button>
          <Button disabled={!count} onClick={() => replace(by, true)}>
            Replace all
          </Button>
        </>
      )}
      <IconButton label="Close find" onClick={close}>
        <X />
      </IconButton>
    </InputRow>
  );
}
