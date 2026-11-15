import "./side-pane.css";
import type { ComponentChildren } from "preact";
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { devicePrefs, type Pane } from "./device-prefs.ts";
import { motionEase, motionMs, useExit } from "./motion.ts";

const rem = () =>
  Number.parseFloat(getComputedStyle(document.documentElement).fontSize);

// Drag it, or focus it and use the arrow keys, to resize; Enter or a
// double-click puts the default back. The size is a root custom property in
// rem, restored before the first paint (public/theme.js).
export function ResizeHandle({
  pane,
  label,
  edge,
}: {
  pane: Pane;
  label: string;
  edge: "start" | "end" | "top";
}) {
  const across = edge === "top";
  const extent = (rect: { width: number; height: number }) =>
    across ? rect.height : rect.width;
  const handle = useRef<HTMLDivElement>(null);
  // The pane's share of its row, for assistive technology.
  const [share, setShare] = useState<number>();
  const box = () => handle.current!.parentElement!.getBoundingClientRect();
  const measure = () => {
    const row = handle.current!.parentElement!.parentElement!;
    const whole = extent({ width: row.clientWidth, height: row.clientHeight });
    setShare(Math.round((extent(box()) / whole) * 100));
  };
  // biome-ignore lint/correctness/useExhaustiveDependencies: measured on mount; a resize measures again
  useLayoutEffect(measure, []);
  const resize = (width: number | null, keep = true) => {
    const style = document.documentElement.style;
    const property = `--${pane}-${across ? "height" : "width"}`;
    const panes = devicePrefs.panes.read();
    if (width === null) {
      style.removeProperty(property);
      delete panes[pane];
    } else {
      style.setProperty(property, `${width / rem()}rem`);
      // The layout stops a pane short of crowding out the chat: keep the
      // size it drew, so dragging back answers at once.
      panes[pane] = extent(box()) / rem();
      style.setProperty(property, `${panes[pane]}rem`);
    }
    measure();
    if (keep) devicePrefs.panes.write(panes);
  };
  const direction = edge === "end" ? 1 : -1;
  const [back, forward] = across
    ? ["ArrowUp", "ArrowDown"]
    : ["ArrowLeft", "ArrowRight"];
  return (
    <div
      ref={handle}
      class={`resize-handle ${edge}`}
      role="separator"
      tabIndex={0}
      aria-orientation={across ? "horizontal" : "vertical"}
      aria-label={label}
      aria-valuenow={share}
      onPointerDown={(event) => {
        if (event.button !== 0) return;
        event.preventDefault();
        const target = event.currentTarget;
        const at = (point: PointerEvent) =>
          across ? point.clientY : point.clientX;
        const start = at(event);
        const from = extent(box());
        const move = (next: PointerEvent) =>
          resize(from + (at(next) - start) * direction, false);
        target.setPointerCapture(event.pointerId);
        target.dataset.dragging = "";
        target.addEventListener("pointermove", move);
        target.addEventListener(
          "lostpointercapture",
          () => {
            delete target.dataset.dragging;
            target.removeEventListener("pointermove", move);
            resize(extent(box()));
          },
          { once: true },
        );
      }}
      onDblClick={() => resize(null)}
      onKeyDown={(event) => {
        const step = (event.shiftKey ? 64 : 16) * direction;
        if (event.key === forward) resize(extent(box()) + step);
        else if (event.key === back) resize(extent(box()) - step);
        else if (event.key === "Enter") resize(null);
        else return;
        event.preventDefault();
      }}
    />
  );
}

// The chat sits in the same place with or without a pane: a new parent
// would remount the transcript and load its history again. The pane's
// content keeps its width while its column grows, so it never reflows.
export function SideLayout({
  pane,
  children,
}: {
  pane?: { name: Pane; label: string; content: ComponentChildren };
  children: ComponentChildren;
}) {
  const [shown, ref] = useExit<typeof pane, HTMLDivElement>(pane);
  const opener = useRef<Element | null>(null);
  const open = !!pane;
  // After useExit's own effect, so its wait for the exit sees this one.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on opening and closing
  useLayoutEffect(() => {
    const element = ref.current;
    const content = element?.lastElementChild;
    const duration = motionMs("base");
    if (!element || !content || !duration) return;
    const width = `${element.getBoundingClientRect().width}px`;
    // The column clips while it moves; the content holds its full width.
    const shut = {
      width: "0px",
      minWidth: "0px",
      opacity: 0,
      overflow: "clip",
    };
    const full = { width, minWidth: "0px", opacity: 1, overflow: "clip" };
    const options = {
      duration,
      easing: motionEase(open ? "out" : "in"),
      fill: open ? ("none" as const) : ("forwards" as const),
    };
    const pane = element.animate(open ? [shut, full] : [full, shut], options);
    const held = { width };
    const inside = content.animate([held, held], options);
    return () => [pane, inside].forEach((run) => run.cancel());
  }, [open, !!shown]);
  useLayoutEffect(() => {
    if (open) {
      opener.current = document.activeElement;
      return;
    }
    const target = opener.current;
    const active = document.activeElement;
    if (
      target instanceof HTMLElement &&
      target.isConnected &&
      (active === document.body || active?.closest(".side-pane"))
    )
      target.focus({ preventScroll: true });
  }, [open]);
  return (
    <div class="side-layout">
      <div class="side-chat">{children}</div>
      {shown && (
        <div ref={ref} class="side-pane" data-pane={shown.name}>
          <ResizeHandle pane={shown.name} label={shown.label} edge="start" />
          {shown.content}
        </div>
      )}
    </div>
  );
}
