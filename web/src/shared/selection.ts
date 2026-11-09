import { useMemo, useState } from "preact/hooks";

// Rows picked to act on together, as a desktop file list picks them: ⌘ or
// Ctrl and a click adds or removes one, Shift and a click adds the run from
// the last one picked, in the order the list shows; the row's checkbox is
// ⌘-click. In select mode (a phone's, started from a row's menu) a tap
// picks too. Esc, Clear or Done end it. The sidebar and the Library each
// keep one; the design is the multi-select canvas.

export interface Selection {
  ids: ReadonlySet<string>;
  // Where a Shift run starts: the row picked last.
  anchor: string;
  // Taps pick instead of open (a phone's select mode).
  mode: boolean;
}
export const NONE: Selection = { ids: new Set(), anchor: "", mode: false };

export const toggled = (state: Selection, id: string): Selection => {
  const ids = new Set(state.ids);
  if (!ids.delete(id)) ids.add(id);
  return { ...state, ids, anchor: id };
};
// Adds the rows from the anchor to `id`, in `order`; from nothing, `id`.
export const ranged = (
  state: Selection,
  id: string,
  order: readonly string[],
): Selection => {
  const to = order.indexOf(id);
  const from = state.anchor ? order.indexOf(state.anchor) : -1;
  if (to < 0) return state;
  if (from < 0) return toggled(state, id);
  const [first, last] = from < to ? [from, to] : [to, from];
  return {
    ...state,
    ids: new Set([...state.ids, ...order.slice(first, last + 1)]),
  };
};
// Only what is still there: a row deleted elsewhere leaves, and when all
// picked went (a bulk delete done) selecting ends, select mode too.
export const kept = (state: Selection, there: readonly string[]) => {
  const ids = [...state.ids].filter((id) => there.includes(id));
  if (ids.length === state.ids.size) return state;
  return ids.length ? { ...state, ids: new Set(ids) } : NONE;
};

// The same object until the selection changes, so a list given it renders
// only then, not with every frame the app draws.
export function useSelection() {
  const [state, setState] = useState<Selection>(NONE);
  return useMemo(
    () => ({
      ids: state.ids,
      mode: state.mode,
      selecting: state.mode || state.ids.size > 0,
      toggle: (id: string) => setState((was) => toggled(was, id)),
      all: (order: readonly string[]) =>
        setState((was) => ({ ...was, ids: new Set(order) })),
      // Select mode from a row's menu: that row picked, taps pick.
      start: (id: string) =>
        setState({ ids: new Set([id]), anchor: id, mode: true }),
      clear: () => setState(NONE),
      // Keeps only rows still in `there`.
      keep: (there: readonly string[]) => setState((was) => kept(was, there)),
      // A click that picks instead of opening; true when it picked.
      click: (id: string, event: MouseEvent, order: readonly string[]) => {
        if (event.shiftKey) setState((was) => ranged(was, id, order));
        else if (event.metaKey || event.ctrlKey || state.mode)
          setState((was) => toggled(was, id));
        else return false;
        event.preventDefault();
        return true;
      },
    }),
    [state],
  );
}
export type SelectionApi = ReturnType<typeof useSelection>;
