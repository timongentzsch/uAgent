import { useId, useRef, useState } from "preact/hooks";
import { ChevronDown } from "lucide-preact";
import { Field, Input } from "./ui.tsx";
import { SuggestionList } from "./suggestion-list.tsx";
import "./search-select.css";

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
  const words = query.toLocaleLowerCase().trim().split(/\s+/);
  const matches = options.filter((option) =>
    words.every((word) =>
      `${option.label} ${option.value} ${option.detail || ""}`
        .toLocaleLowerCase()
        .includes(word),
    ),
  );
  const active = Math.min(index, matches.length - 1);
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
          onFocus={(event) => event.currentTarget.select()}
          onClick={() => {
            setOpen(true);
            setIndex(0);
          }}
          onInput={(event) => {
            setQuery(event.currentTarget.value);
            setOpen(true);
            setIndex(0);
          }}
          onKeyDown={(event) => {
            if (event.isComposing) return;
            if (event.key === "ArrowDown" || event.key === "ArrowUp") {
              event.preventDefault();
              setOpen(true);
              setIndex(
                open
                  ? Math.max(
                      0,
                      Math.min(
                        matches.length - 1,
                        active + (event.key === "ArrowDown" ? 1 : -1),
                      ),
                    )
                  : 0,
              );
            } else if (open && event.key === "Enter") {
              event.preventDefault();
              if (matches[active]) pick(matches[active]);
            } else if (open && event.key === "Escape") {
              event.preventDefault();
              event.stopPropagation();
              setOpen(false);
              setQuery("");
            }
          }}
        />
      </Field>
      <ChevronDown class="search-select-chevron" aria-hidden="true" />
      {open && !disabled && (
        <div class="search-select-options">
          <SuggestionList
            id={id}
            label={`${label} options`}
            prefix={`${id}-option`}
            items={matches}
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
          {!matches.length && <p role="status">No matching options.</p>}
        </div>
      )}
    </div>
  );
}
