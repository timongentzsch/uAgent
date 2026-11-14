import { ChevronDown, ChevronUp, X } from "lucide-preact";
import { useEffect, useRef } from "preact/hooks";
import { IconButton, Input } from "./ui.tsx";

// What is searched, and how an occurrence is shown, is the caller's.
export function FindField({
  label,
  words,
  setWords,
  count,
  current,
  step,
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
  close: () => void;
}) {
  const bar = useRef<HTMLDivElement>(null);
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
    <div class="find-bar" role="search" aria-label={label} ref={bar}>
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
      <IconButton label="Close find" onClick={close}>
        <X />
      </IconButton>
    </div>
  );
}
