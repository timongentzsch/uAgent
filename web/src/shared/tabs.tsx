import type { ComponentChildren, Ref } from "preact";
import { Button } from "./ui.tsx";

// A tab list. Left, Right, Home and End go among the tabs that are not
// disabled and take the focus along; only the selected tab is a Tab stop.
export function Tabs<K extends string | number>({
  label,
  tabs,
  value,
  to,
  id,
  panel,
  size,
  class: extra = "",
  listRef,
}: {
  label: string;
  tabs: {
    key: K;
    label: ComponentChildren;
    title?: string;
    disabled?: boolean;
  }[];
  value: K;
  to: (key: K) => void;
  // A tab's element is `${id}-${key}`, which its panel is labelled by.
  id?: string;
  panel?: string;
  size?: "compact";
  class?: string;
  listRef?: Ref<HTMLDivElement>;
}) {
  return (
    <div
      class={`tabs ${extra}`}
      role="tablist"
      aria-label={label}
      ref={listRef}
      onKeyDown={(event) => {
        const open = tabs.flatMap((tab, at) => (tab.disabled ? [] : [at]));
        const at = open.indexOf(tabs.findIndex((tab) => tab.key === value));
        const next = {
          ArrowLeft: open[at - 1],
          ArrowRight: open[at + 1],
          Home: open[0],
          End: open.at(-1),
        }[event.key];
        if (next === undefined) return;
        event.preventDefault();
        to(tabs[next].key);
        (event.currentTarget.children[next] as HTMLElement).focus();
      }}
    >
      {tabs.map((tab) => (
        <Button
          key={tab.key}
          size={size}
          id={id && `${id}-${tab.key}`}
          role="tab"
          title={tab.title}
          aria-controls={panel}
          aria-selected={tab.key === value}
          tabIndex={tab.key === value ? 0 : -1}
          disabled={tab.disabled}
          onClick={() => to(tab.key)}
        >
          {tab.label}
        </Button>
      ))}
    </div>
  );
}
