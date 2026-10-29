import "./suggestion-list.css";
import { useId, useLayoutEffect, useRef } from "preact/hooks";
import type { ComponentChildren } from "preact";
import { Button } from "./ui.tsx";

// Options under a field that keeps focus and names the active option
// (`${prefix}-${position}`) as its active descendant; the active option
// stays in view. By default they pop up over the composer.
export function SuggestionList<T>({
  id,
  label,
  prefix,
  items,
  index,
  pick,
  keyOf,
  groupOf,
  class: className = "command-suggestions popover-list",
  children,
}: {
  id: string;
  label: string;
  prefix: string;
  items: T[];
  index: number;
  pick: (item: T) => void;
  keyOf?: (item: T) => string;
  // A heading for each run of items with the same label; options keep their
  // position as their id, so the field names them as before.
  groupOf?: (item: T) => string | undefined;
  class?: string;
  children: (item: T) => ComponentChildren;
}) {
  const list = useRef<HTMLDivElement>(null);
  const groupId = useId();
  // Follows the option, not the render: a caller that rebuilds its items on
  // every render (the palette, while a turn streams) must not pull a list the
  // person scrolled back to its active option each time.
  const current = items[index];
  useLayoutEffect(() => {
    list.current
      ?.querySelector('[aria-selected="true"]')
      ?.scrollIntoView({ block: "nearest" });
  }, [index, current && keyOf ? keyOf(current) : current]);
  return (
    <div
      id={id}
      class={`suggestions ${className}`}
      role="listbox"
      aria-label={label}
      ref={list}
    >
      {runs(items, groupOf).map(({ label, start, run }) => {
        const options = run.map((item, offset) => (
          <Button
            key={keyOf?.(item)}
            variant="quiet"
            class="entry"
            role="option"
            id={`${prefix}-${start + offset}`}
            aria-selected={start + offset === index}
            tabIndex={-1}
            onMouseDown={(event) => event.preventDefault()}
            onClick={() => pick(item)}
          >
            {children(item)}
          </Button>
        ));
        if (!label) return options;
        const heading = `${groupId}-${start}`;
        return (
          <div role="group" aria-labelledby={heading} key={heading}>
            <div class="suggestion-group caption" id={heading}>
              {label}
            </div>
            {options}
          </div>
        );
      })}
    </div>
  );
}

// Items split where their group label changes, each run with its position.
function runs<T>(items: T[], groupOf?: (item: T) => string | undefined) {
  const out: { label?: string; start: number; run: T[] }[] = [];
  items.forEach((item, position) => {
    const label = groupOf?.(item);
    const last = out.at(-1);
    if (last && last.label === label) last.run.push(item);
    else out.push({ label, start: position, run: [item] });
  });
  return out;
}
