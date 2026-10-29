import { useId, useRef, useState } from "preact/hooks";
import { ChevronDown } from "lucide-preact";
import { EmptyState, Field, Input } from "./ui.tsx";
import { SuggestionList } from "./suggestion-list.tsx";
import "./search-select.css";
import { matches } from "./fuzzy.ts";
import { nextIndex, plainKey } from "./listbox-nav.ts";

// A select with a search box, for long lists such as models.

export type SearchOption = { value: string; label: string; detail?: string };

// A closed choice stays selected while its search text is edited. Only
// picking an option changes the value handed to the owner.
export function SearchSelect({
  label,
  value,
  options,
  disabled,
  change,
}: {
  label: string;
  value: string;
  options: SearchOption[];
  disabled?: boolean;
  change: (value: string) => void;
}) {
  const id = useId();
  const root = useRef<HTMLDivElement>(null);
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState("");
  const [index, setIndex] = useState(0);
  const selected = options.find((option) => option.value === value);
  const shown = options.filter((option) =>
    matches(query, option.label, option.value, option.detail),
  );
  const active = Math.min(index, shown.length - 1);
  const search = () => {
    setOpen(true);
    setIndex(0);
  };
  const pick = (option: SearchOption) => {
    change(option.value);
    setOpen(false);
    setQuery("");
    root.current?.querySelector("input")?.focus({ preventScroll: true });
  };
  return (
    <div
      class="search-select"
      ref={root}
      onFocusOut={(event) => {
        if (!root.current?.contains(event.relatedTarget as Node | null)) {
          setOpen(false);
          setQuery("");
        }
      }}
    >
      <Field label={label}>
        <Input
          role="combobox"
          aria-label={label}
          aria-autocomplete="list"
          aria-expanded={open && !disabled}
          aria-controls={open ? id : undefined}
          aria-activedescendant={
            open && active >= 0 ? `${id}-option-${active}` : undefined
          }
          value={open ? query : selected?.label || value}
          placeholder={`Search ${label.toLowerCase()}…`}
          disabled={disabled}
          autocomplete="off"
          autocapitalize="off"
          spellcheck={false}
          onFocus={search}
          onClick={search}
          onInput={(event) => {
            setQuery(event.currentTarget.value);
            search();
          }}
          onKeyDown={(event) => {
            if (event.isComposing || !plainKey(event)) return;
            if (event.key === "ArrowDown" || event.key === "ArrowUp") {
              setOpen(true);
              setIndex(
                open && shown.length
                  ? nextIndex(active, event.key, shown.length)
                  : 0,
              );
            } else if (open && event.key === "Enter") {
              if (shown[active]) pick(shown[active]);
            } else if (open && event.key === "Escape") {
              event.stopPropagation();
              setOpen(false);
              setQuery("");
            } else return;
            event.preventDefault();
          }}
        />
      </Field>
      <ChevronDown class="search-select-chevron" aria-hidden="true" />
      {open && !disabled && (
        <div class="search-select-options popover-list">
          <SuggestionList
            id={id}
            label={`${label} options`}
            prefix={`${id}-option`}
            items={shown}
            index={active}
            pick={pick}
            keyOf={(option) => option.value}
            class="search-select-list"
          >
            {(option) => (
              <span>
                <span>{option.label}</span>
                {option.detail && <small>{option.detail}</small>}
              </span>
            )}
          </SuggestionList>
          {!shown.length && <EmptyState>No matching options.</EmptyState>}
        </div>
      )}
    </div>
  );
}
