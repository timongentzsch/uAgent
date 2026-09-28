import { useLayoutEffect, useRef } from "preact/hooks";

// Open layers (dialogs, sheets, inner pages) as one stack bound to browser
// history, so the system back gesture closes the topmost layer as it does in
// a native app. Each layer owns one history entry carrying its depth; session
// navigation (the #session= hash) keeps its own entries and listener.
interface Layer {
  depth: number;
  close: () => void;
  // Set when history already left this layer's entry.
  popped: boolean;
}

const stack: Layer[] = [];
// history.back() calls issued by layers closing themselves; their popstate
// is ours to swallow.
let pending = 0;

type Entry = { layer?: number; seq?: number } | null;
const depthOf = (state: unknown) => (state as Entry)?.layer ?? 0;
// Entries the app pushes are numbered as they are created, so a move from a
// newer one to an older one is known to be a step back.
const seqOf = (state: unknown) => (state as Entry)?.seq;
let seq = Date.now();
let at = seqOf(history.state);
// The state for any new entry the app pushes (a session, a layer).
export const entry = (layer?: number) => ({ layer, seq: (at = ++seq) });

// A reload or deep link can land on a layer entry with nothing open.
if (depthOf(history.state)) history.replaceState(null, "");

addEventListener("popstate", (event) => {
  if (pending) {
    pending--;
    at = seqOf(event.state);
    return;
  }
  // Back closes every layer above the entry it landed on; forward never
  // reopens one.
  const depth = depthOf(event.state);
  while (stack.length && stack[stack.length - 1].depth > depth) {
    const layer = stack.pop()!;
    layer.popped = true;
    layer.close();
  }
  // An entry whose layer is gone (a dialog swapped for another, a drawer
  // left for a session) is stepped past in the direction of travel, so one
  // press always reaches something visible.
  const from = at;
  at = seqOf(event.state);
  const dead = depth > (stack[stack.length - 1]?.depth ?? 0);
  if (!dead || from === undefined || at === undefined) return;
  if (at < from) sync();
  else history.go(1);
});

// Layers closed by the interface leave their entries behind; one pass after
// the render removes them together, in a single history step. It only acts
// while the current entry is one of ours: a session opened from inside a
// layer has pushed its own entry, which must stay.
let syncing = 0;
function sync() {
  if (syncing) return;
  syncing = requestAnimationFrame(() => {
    syncing = 0;
    const current = depthOf(history.state);
    const wanted = stack[stack.length - 1]?.depth ?? 0;
    if (current > wanted) {
      pending++;
      history.go(wanted - current);
    }
  });
}

// While `active`, the layer sits on the stack and back calls `close`.
export function useDismiss(active: boolean, close: () => void) {
  const latest = useRef(close);
  latest.current = close;
  // Layout effect: the entry exists as soon as the layer is on screen, so a
  // quick back can never skip past it.
  useLayoutEffect(() => {
    if (!active) return;
    const top = (stack[stack.length - 1]?.depth ?? 0) + 1;
    const current = depthOf(history.state);
    // A layer that replaces ones closing in the same render (a dialog
    // swapped for another) takes over the current entry and its depth, so
    // back and the sync pass count every entry that is really there.
    const reuse = current >= top;
    const layer: Layer = {
      depth: reuse ? current : top,
      close: () => latest.current(),
      popped: false,
    };
    stack.push(layer);
    history[reuse ? "replaceState" : "pushState"](entry(layer.depth), "");
    return () => {
      const index = stack.indexOf(layer);
      if (index >= 0) stack.splice(index, 1);
      if (!layer.popped) sync();
    };
  }, [active]);
}
