import type { ComponentChildren } from "preact";
import { useEffect, useLayoutEffect, useRef } from "preact/hooks";
import { Menu, MenuItem } from "./menu.tsx";
import type { Rect } from "./page-frame.tsx";
import { IconButton } from "./ui.tsx";
import { useLatest } from "./use-latest.ts";
import "./float-bars.css";

// Bars that float over a page on its stage: the tools that put something
// on it, and the controls for what is picked there.

// A tool, or one with kinds, which opens a menu of them; a kind is armed by
// its own name.
export interface Tool {
  name: string;
  label: string;
  icon: ComponentChildren;
  kinds?: [string, string][];
}

// At the foot of the stage: one tool is armed at a time, the first (which
// picks) when none is.
export function InsertBar({
  tools,
  armed,
  arm,
}: {
  tools: Tool[];
  armed: string;
  arm: (name: string) => void;
}) {
  return (
    <div
      class="float-toolbar segmented foot-bar"
      role="group"
      aria-label="Insert"
    >
      {tools.map(({ name, label, icon, kinds }) =>
        kinds ? (
          <Menu
            key={name}
            label={label}
            pressed={kinds.some(([kind]) => kind === armed)}
            trigger={
              <>
                {icon}
                <span class="sr-only">{label}</span>
              </>
            }
          >
            {kinds.map(([kind, named]) => (
              <MenuItem key={kind} onClick={() => arm(kind)}>
                {named}
              </MenuItem>
            ))}
          </Menu>
        ) : (
          <IconButton
            key={name}
            label={label}
            aria-pressed={armed === name}
            onClick={() => arm(name)}
          >
            {icon}
          </IconButton>
        ),
      )}
    </div>
  );
}

// Over what is picked, `at` in the window: above it, below it where there
// is no room above (or, `below`, the other way round), and inside the stage
// it floats in, short of the scroll bars of what scrolls there (`room`, its
// client size).
export function PickBar({
  label,
  at,
  room,
  below,
  children,
}: {
  label: string;
  at: () => Rect | undefined;
  room?: () => { width: number; height: number } | undefined;
  below?: boolean;
  children: ComponentChildren;
}) {
  const bar = useRef<HTMLDivElement>(null);
  const place = useLatest(() => {
    const node = bar.current;
    const stage = node?.offsetParent?.getBoundingClientRect();
    const rect = at();
    if (!node || !stage || !rect) return;
    // Its margins keep it off the item and the stage's edges.
    const { marginLeft, marginTop } = getComputedStyle(node);
    const w = node.offsetWidth + 2 * Number.parseFloat(marginLeft);
    const h = node.offsetHeight + 2 * Number.parseFloat(marginTop);
    const { width, height } = room?.() ?? stage;
    const middle = rect.x + rect.width / 2 - stage.left;
    const above = rect.y - stage.top - h;
    const under = rect.y + rect.height - stage.top;
    const x = Math.max(0, Math.min(middle - w / 2, width - w));
    const y = Math.max(
      0,
      below && under <= height - h
        ? under
        : above >= 0
          ? above
          : Math.min(under, height - h),
    );
    node.style.setProperty("--bar-x", `${x}px`);
    node.style.setProperty("--bar-y", `${y}px`);
  });
  useLayoutEffect(() => place.current());
  useEffect(() => {
    const moved = () => place.current();
    addEventListener("scroll", moved, { capture: true, passive: true });
    addEventListener("resize", moved);
    return () => {
      removeEventListener("scroll", moved, { capture: true });
      removeEventListener("resize", moved);
    };
  }, [place]);
  return (
    <div
      class="float-toolbar pick-bar"
      role="toolbar"
      aria-label={label}
      ref={bar}
      // A press leaves the focus, and so the selection, in the page.
      onMouseDown={(event) => event.preventDefault()}
    >
      {children}
    </div>
  );
}
