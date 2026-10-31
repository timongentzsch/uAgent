import { folderOf } from "../../shared/display.ts";
import { MenuItem } from "../../shared/menu.tsx";
import type { Pin, Session, Space } from "../../shared/types.ts";
import { command } from "../../state/api.ts";
import { useHostContext } from "../../state/host-context.ts";

const same = (a: Pin, b: Pin) => a.kind === b.kind && a.id === b.id;

// The pins whose conversation or folder the catalogue still lists: the only
// ones a list is written with, so the host's own cleans itself.
export function livePins(
  pins: Pin[] = [],
  sessions: Session[],
  spaces: Space[] = [],
) {
  const ids = new Set(sessions.map((item) => item.id));
  const folders = new Set([
    ...sessions.map(folderOf),
    ...spaces.map((space) => space.path),
  ]);
  return pins.filter((pin) =>
    (pin.kind === "session" ? ids : folders).has(pin.id),
  );
}

// Keeps a conversation or a folder at the top of the list, on every device.
export function PinItem({ pin }: { pin: Pin }) {
  const { online, pins, report } = useHostContext();
  const pinned = pins.some((item) => same(item, pin));
  return (
    <MenuItem
      disabled={!online}
      onClick={() =>
        void command("pins", null, {
          pins: pinned
            ? pins.filter((item) => !same(item, pin))
            : [...pins, pin],
        }).catch(report)
      }
    >
      {pinned ? "Unpin" : "Pin"}
    </MenuItem>
  );
}
