import { useState } from "preact/hooks";
import { Check } from "lucide-preact";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { ConfirmModal } from "../../shared/ui.tsx";
import { duplicated, type Move, moved, removed } from "./boards.ts";

// A deck's slide moved, duplicated, added to, skipped or deleted from its
// menu, which stands beside the counter, not in the legend: a listbox holds
// only its options.
export default function SlideActions({
  slides,
  index,
  title,
  skipped,
  busy,
  act,
  add,
  skip,
}: {
  // The deck's slides by name, and the one the menu is for.
  slides: string[];
  index: number;
  title: string;
  skipped?: boolean;
  busy?: boolean;
  act: (moves: Move[]) => void;
  // A new slide after this one; this one skipped, or no longer.
  add: () => void;
  skip: () => void;
}) {
  const [deleting, setDeleting] = useState(false);
  const items: [string, boolean, () => void][] = [
    ["Move earlier", !index, () => act(moved(slides, index, index - 1))],
    [
      "Move later",
      index === slides.length - 1,
      () => act(moved(slides, index, index + 1)),
    ],
    ["Duplicate", false, () => act(duplicated(slides, index))],
    ["Add slide after", false, add],
  ];
  return (
    <>
      <Menu label="Slide actions">
        {items.map(([name, off, run]) => (
          <MenuItem key={name} disabled={busy || off} onClick={run}>
            {name}
          </MenuItem>
        ))}
        <MenuItem
          role="menuitemcheckbox"
          aria-checked={!!skipped}
          disabled={busy}
          onClick={skip}
        >
          Skip slide
          {skipped && <Check />}
        </MenuItem>
        <MenuItem disabled={busy} onClick={() => setDeleting(true)}>
          Delete…
        </MenuItem>
      </Menu>
      {deleting && (
        <ConfirmModal
          title="Delete slide"
          close={() => setDeleting(false)}
          confirm={() => {
            setDeleting(false);
            act(removed(slides, index));
          }}
        >
          {`${title} (${slides[index]}) is deleted from the folder. This cannot be undone.`}
        </ConfirmModal>
      )}
    </>
  );
}
