// Development only, like ui.html: nothing in the product imports this. A
// browser test imports it into the Vite-served app, where it shares the
// app's Preact instance, and reads how often each component rendered.
import { options, type VNode } from "preact";

type Hooked = typeof options & { __r?: (vnode: VNode) => void };
const hooked = options as Hooked;
const counts: Record<string, number> = {};
const previous = hooked.__r;
// Preact's render hook (mangled `_render`) runs only for components that
// actually render, not those an equality check skipped.
hooked.__r = (vnode) => {
  const type = vnode.type as { displayName?: string; name?: string };
  const name =
    typeof vnode.type === "function" && (type.displayName || type.name);
  if (name) counts[name] = (counts[name] || 0) + 1;
  previous?.(vnode);
};
(globalThis as { renderCounts?: typeof counts }).renderCounts = counts;
