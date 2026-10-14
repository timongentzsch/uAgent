import "./suggestion-list.css";
import { useLayoutEffect, useRef } from "preact/hooks";
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
  class: className = "command-suggestions",
  children,
}: {
  id: string;
  label: string;
  prefix: string;
  items: T[];
  index: number;
  pick: (item: T) => void;
  keyOf?: (item: T) => string;
  class?: string;
  children: (item: T) => ComponentChildren;
}) {
  const list = useRef<HTMLDivElement>(null);
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
      {items.map((item, position) => (
        <Button
          key={keyOf?.(item)}
          variant="quiet"
          role="option"
          id={`${prefix}-${position}`}
          aria-selected={position === index}
          tabIndex={-1}
          onMouseDown={(event) => event.preventDefault()}
          onClick={() => pick(item)}
        >
          {children(item)}
        </Button>
      ))}
    </div>
  );
}
