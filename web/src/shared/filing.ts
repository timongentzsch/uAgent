import type { PlaceOption, Session, Space } from "./types.ts";
import { folderOf } from "./display.ts";
import { folderName } from "./folder-label.tsx";
import { scratchSpace, unfiledDays } from "./limits.ts";

// Which conversations in Scratch wait for a place, and how a list names the
// place suggested for one.
const DAY = 24 * 60 * 60 * 1000;

export const optionsOf = (item: Session) =>
  item.suggestion?.dismissed ? [] : item.suggestion?.options || [];

export const nameOf = (option: PlaceOption, spaces: Space[]) =>
  option.workspace ||
  spaces.find((space) => space.path === option.cwd)?.name ||
  folderName(option.cwd);

// What a list says of a suggestion in a few words.
export const filingTag = (item: Session, spaces: Space[]) => {
  const options = optionsOf(item);
  if (!options.length) return undefined;
  return options[0].workspace
    ? "→ new workspace"
    : `→ ${options.map((option) => nameOf(option, spaces)).join(" or ")}`;
};

// Scratch's conversations that still wait for a place: the suggested ones,
// and for a week the ones nothing was suggested for.
export function unfiled(sessions: Session[], spaces: Space[]) {
  const scratch = spaces.find((space) => space.name === scratchSpace)?.path;
  if (!scratch) return [];
  return sessions
    .filter(
      (item) =>
        !item.parent &&
        !item.host &&
        folderOf(item) === scratch &&
        !item.suggestion?.dismissed &&
        (optionsOf(item).length > 0 ||
          Date.now() - (item.updated || 0) < unfiledDays * DAY),
    )
    .sort((a, b) => (b.updated || 0) - (a.updated || 0));
}
