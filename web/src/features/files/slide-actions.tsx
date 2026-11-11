import { useState } from "preact/hooks";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { ConfirmModal } from "../../shared/ui.tsx";
import { duplicated, type Move, moved, removed } from "./boards.ts";

// A deck's slide moved, duplicated or deleted from its menu, which stands
// beside the counter, not in the legend: a listbox holds only its options.
export default function SlideActions({
  slides,
  index,
  title,
  busy,
  act,
}: {
  // The deck's slides by name, and the one the menu is for.
  slides: string[];
  index: number;
  title: string;
  busy?: boolean;
  act: (moves: Move[]) => void;
}) {
  const [deleting, setDeleting] = useState(false);
  const items: [string, boolean, () => void][] = [
    ["Move up", !index, () => act(moved(slides, index, index - 1))],
    [
      "Move down",
      index === slides.length - 1,
      () => act(moved(slides, index, index + 1)),
    ],
    ["Duplicate", false, () => act(duplicated(slides, index))],
    ["Delete…", false, () => setDeleting(true)],
  ];
  return (
    <>
      <Menu label="Slide actions">
        {items.map(([name, off, run]) => (
          <MenuItem key={name} disabled={busy || off} onClick={run}>
            {name}
          </MenuItem>
        ))}
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
