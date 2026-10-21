import "./side-pane.css";
import type { ComponentChildren } from "preact";
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { devicePrefs, type Pane } from "./device-prefs.ts";

const rem = () =>
  Number.parseFloat(getComputedStyle(document.documentElement).fontSize);

// The edge of a docked pane: drag it, or focus it and use the arrow keys
// (Shift for bigger steps), to resize the pane; Enter or a double-click
// puts the default back. The width is a custom property on the root, kept
// per device and restored before the first paint (public/theme.js). `edge`
// is the side of the pane the handle sits on.
export function ResizeHandle({
  pane,
  label,
  edge,
}: {
  pane: Pane;
  label: string;
  edge: "start" | "end";
}) {
  const handle = useRef<HTMLDivElement>(null);
  // The pane's share of its row, for assistive technology.
  const [share, setShare] = useState<number>();
  const box = () => handle.current!.parentElement!.getBoundingClientRect();
  const measure = () =>
    setShare(
      Math.round(
        (box().width /
          handle.current!.parentElement!.parentElement!.clientWidth) *
          100,
      ),
    );
  // biome-ignore lint/correctness/useExhaustiveDependencies: measured on mount; a resize measures again
  useLayoutEffect(measure, []);
  const resize = (width: number | null, keep = true) => {
    const style = document.documentElement.style;
    const property = `--${pane}-width`;
    const panes = devicePrefs.panes.read();
    if (width === null) {
      style.removeProperty(property);
      delete panes[pane];
    } else {
      style.setProperty(property, `${width / rem()}rem`);
      // The layout stops a pane short of crowding out the chat: keep the
      // width it drew, so dragging back answers at once.
      panes[pane] = box().width / rem();
      style.setProperty(property, `${panes[pane]}rem`);
    }
    measure();
    if (keep) devicePrefs.panes.write(panes);
  };
  const direction = edge === "start" ? -1 : 1;
  return (
    <div
      ref={handle}
      class={`resize-handle ${edge}`}
      role="separator"
      tabIndex={0}
      aria-orientation="vertical"
      aria-label={label}
      aria-valuenow={share}
      onPointerDown={(event) => {
        if (event.button !== 0) return;
        event.preventDefault();
        const target = event.currentTarget;
        const start = event.clientX;
        const from = box().width;
        const move = (next: PointerEvent) =>
          resize(from + (next.clientX - start) * direction, false);
        target.setPointerCapture(event.pointerId);
        target.dataset.dragging = "";
        target.addEventListener("pointermove", move);
        target.addEventListener(
          "lostpointercapture",
          () => {
            delete target.dataset.dragging;
            target.removeEventListener("pointermove", move);
            resize(box().width);
          },
          { once: true },
        );
      }}
      onDblClick={() => resize(null)}
      onKeyDown={(event) => {
        const step = (event.shiftKey ? 64 : 16) * direction;
        if (event.key === "ArrowRight") resize(box().width + step);
        else if (event.key === "ArrowLeft") resize(box().width - step);
        else if (event.key === "Enter") resize(null);
        else return;
        event.preventDefault();
      }}
    />
  );
}

// A conversation with a pane docked beside it (a panel's board, the
// files): the chat keeps the rest of the row. The chat sits in the same
// place with or without a pane, so opening one only narrows it: a new
// parent would remount the transcript and load its history again.
export function SideLayout({
  pane,
  children,
}: {
  pane?: { name: Pane; label: string; content: ComponentChildren };
  children: ComponentChildren;
}) {
  return (
    <div class="side-layout">
      <div class="side-chat">{children}</div>
      {pane && (
        <div class="side-pane" data-pane={pane.name}>
          <ResizeHandle pane={pane.name} label={pane.label} edge="start" />
          {pane.content}
        </div>
      )}
    </div>
  );
}
