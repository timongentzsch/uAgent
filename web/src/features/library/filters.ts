import { matches } from "../../shared/fuzzy.ts";
import type {
  LibraryItem,
  LibraryKind,
  LibraryPlace,
} from "../../shared/types.ts";

export const allPlaces = "*";
export const placeId = (place: LibraryPlace) => place.key ?? place.path;

// Each filter shows how many items it would reveal with the other filter
// still applied. Group rows in the same order as the place picker.
export function filterLibrary(
  items: LibraryItem[],
  places: Map<string, LibraryPlace>,
  kind: LibraryKind | "",
  where: string,
  query: string,
) {
  const kinds = new Map<string, number>();
  const locations = new Map<string, number>();
  const rows = new Map<string, LibraryItem[]>();
  for (const item of items) {
    const place = item.place ?? "";
    if (
      query &&
      !matches(query, item.name, item.description, places.get(place)?.name)
    )
      continue;
    const atPlace = where === allPlaces || where === place;
    const ofKind = !kind || kind === item.kind;
    if (atPlace) {
      kinds.set("", (kinds.get("") ?? 0) + 1);
      if (item.kind) kinds.set(item.kind, (kinds.get(item.kind) ?? 0) + 1);
    }
    if (!ofKind) continue;
    locations.set(allPlaces, (locations.get(allPlaces) ?? 0) + 1);
    locations.set(place, (locations.get(place) ?? 0) + 1);
    if (atPlace) {
      const group = rows.get(place);
      if (group) group.push(item);
      else rows.set(place, [item]);
    }
  }
  const groups = [...places.keys(), ""].flatMap((path) => {
    const group = rows.get(path);
    return group ? [{ path, rows: group }] : [];
  });
  return { groups, kinds, locations };
}
