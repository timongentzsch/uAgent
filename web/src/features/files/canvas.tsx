import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { TriangleAlert } from "lucide-preact";
import { Button } from "../../shared/ui.tsx";
import { ZoomControls } from "../../shared/zoom-surface.tsx";
import {
  capturePointer,
  distance,
  midpoint,
  pinchView,
  type Point,
  TAP_SLOP_PX,
  type ZoomView,
} from "../../shared/zoom.ts";
import Frame, {
  type PageInfo,
  type PageMessage,
  pageSize,
} from "../../shared/page-frame.tsx";

// A folder's HTML files on a plane you pan and zoom, each a live page: a
// design as the agent wrote it. A page's step (`02-` in
// `02-pricing--annual.html`) is its row; pages without one form the first.
// Pinch, ⌘ or Ctrl and the wheel, or a
// trackpad pinch zoom at the pointer; a drag or the wheel pans. A click opens
// a page; a page that fails says so and offers its errors to the agent.

// The plane: artboards at their real size, 80 px apart in a row and 240
// between rows (room for a name at any zoom), as Claude's canvas spaces them.
const GAP_X = 80;
const GAP_Y = 240;
const ZOOM: [number, number] = [0.05, 2];
// What one message may carry of the pages' errors.
const ERRORS_MAX = 1800;

// A page's step, its leading number (`02-pricing--annual.html` is step
// 02); the variants and states of a step share its row.
const rowOf = (name: string) => /^(\d+)-/.exec(name)?.[1] ?? "";

// The page's files grouped into rows, rows and files in name order.
export function rows(names: string[]) {
  const sorted = [...names].sort((a, b) =>
    a.localeCompare(b, undefined, { numeric: true }),
  );
  const groups = new Map<string, string[]>();
  for (const name of sorted.filter((name) => !rowOf(name)))
    groups.set("", [...(groups.get("") ?? []), name]);
  for (const name of sorted.filter(rowOf))
    groups.set(rowOf(name), [...(groups.get(rowOf(name)) ?? []), name]);
  return [...groups];
}

interface Seen {
  info?: PageInfo;
  errors: string[];
}

export default function Canvas({
  dir,
  names,
  page,
  open,
  addToChat,
}: {
  dir: string;
  names: string[];
  // A file's preview address, with its version.
  page: (path: string) => string;
  open: (path: string) => void;
  addToChat?: (text: string) => void;
}) {
  const [seen, setSeen] = useState<Record<string, Seen>>({});
  const grouped = useMemo(() => rows(names), [names]);
  const path = (name: string) => (dir ? `${dir}/${name}` : name);
  const heard = (name: string) => (message: PageMessage) =>
    setSeen((all) => {
      const was = all[name] ?? { errors: [] };
      const next =
        message.type === "ready"
          ? { info: message, errors: was.errors }
          : message.type === "size"
            ? was.info && {
                ...was,
                info: { ...was.info, height: message.height },
              }
            : message.type === "error" && {
                ...was,
                errors: [
                  ...was.errors,
                  `${message.message}${message.line ? ` (line ${message.line})` : ""}`,
                ],
              };
      return next ? { ...all, [name]: next } : all;
    });
  const stage = useRef<HTMLDivElement>(null);
  const sizeOf = (name: string) => pageSize(seen[name]?.info);
  // Every page's place on the plane.
  // biome-ignore lint/correctness/useExhaustiveDependencies: sizes come from seen
  const placed = useMemo(() => {
    let y = 0;
    return grouped.flatMap(([row, files]) => {
      let x = 0;
      const top = y;
      const items = files.map((name) => {
        const size = sizeOf(name);
        const item = { name, row, x, y: top, ...size };
        x += size.w + GAP_X;
        return item;
      });
      y += Math.max(...items.map((item) => item.h)) + GAP_Y;
      return items;
    });
  }, [grouped, seen]);
  const [view, setView] = useState<ZoomView>({ x: 24, y: 48, scale: 0.2 });
  const fit = () => {
    const box = stage.current;
    if (!box || !placed.length) return;
    const w = Math.max(...placed.map((item) => item.x + item.w));
    const h = Math.max(...placed.map((item) => item.y + item.h));
    const scale = Math.min(
      1,
      (box.clientWidth - 48) / w,
      (box.clientHeight - 96) / h,
    );
    setView({ x: (box.clientWidth - w * scale) / 2, y: 48, scale });
  };
  // Fit once the pages have said how large they are, and for a new folder.
  const sized = names.every((name) => seen[name]?.info);
  // biome-ignore lint/correctness/useExhaustiveDependencies: on arrival
  useEffect(fit, [sized, dir]);
  // Scale by `by`, what was under `from` moving to `to` (by default, staying).
  const zoomAt = (by: number, from: Point, to = from) =>
    setView((was) => pinchView(was, 0, 0, from, to, by, ZOOM));
  const center = (by: number) => {
    const box = stage.current;
    if (box) zoomAt(by, { x: box.clientWidth / 2, y: box.clientHeight / 2 });
  };
  // One pointer pans, two pinch. A pointer that moved opens nothing.
  const pointers = useRef(new Map<number, Point>());
  // The wheel pans; with ⌘ or Ctrl (a trackpad pinch sends Ctrl) it zooms.
  // biome-ignore lint/correctness/useExhaustiveDependencies: one listener, state by updater
  useEffect(() => {
    const box = stage.current;
    if (!box) return;
    const wheel = (event: WheelEvent) => {
      event.preventDefault();
      const at = box.getBoundingClientRect();
      if (event.ctrlKey || event.metaKey)
        zoomAt(Math.exp(-event.deltaY * 0.01), {
          x: event.clientX - at.left,
          y: event.clientY - at.top,
        });
      else
        setView((v) => ({
          ...v,
          x: v.x - event.deltaX,
          y: v.y - event.deltaY,
        }));
    };
    // Safari sends a trackpad pinch as gesture events, not the wheel. A
    // touch pinch sends them too, which the pointers below already zoom.
    let scale = 1;
    const gesture = (event: Event) => {
      const {
        scale: next,
        clientX,
        clientY,
      } = event as Event & {
        scale: number;
        clientX: number;
        clientY: number;
      };
      event.preventDefault();
      if (event.type === "gesturestart") scale = 1;
      else if (pointers.current.size < 2) {
        const at = box.getBoundingClientRect();
        zoomAt(next / scale, { x: clientX - at.left, y: clientY - at.top });
      }
      scale = next;
    };
    box.addEventListener("wheel", wheel, { passive: false });
    for (const kind of ["gesturestart", "gesturechange"])
      box.addEventListener(kind, gesture);
    return () => {
      box.removeEventListener("wheel", wheel);
      for (const kind of ["gesturestart", "gesturechange"])
        box.removeEventListener(kind, gesture);
    };
  }, []);
  const moved = useRef(false);
  const down = (event: PointerEvent) => {
    pointers.current.set(event.pointerId, {
      x: event.clientX,
      y: event.clientY,
    });
    if (pointers.current.size === 1) moved.current = false;
  };
  const move = (event: PointerEvent) => {
    const was = pointers.current.get(event.pointerId);
    if (!was) return;
    const all = [...pointers.current.values()];
    const now = { x: event.clientX, y: event.clientY };
    if (distance([now, was]) > TAP_SLOP_PX) moved.current = true;
    if (!moved.current) return;
    capturePointer(event.currentTarget as HTMLElement, event.pointerId);
    if (all.length === 2) {
      // What was under the fingers' midpoint stays under it, scaled by
      // how far apart they moved.
      const other = all.find((point) => point !== was)!;
      const at = stage.current!.getBoundingClientRect();
      const local = (point: Point) => ({
        x: point.x - at.left,
        y: point.y - at.top,
      });
      zoomAt(
        distance([now, other]) / (distance([was, other]) || 1),
        local(midpoint([was, other])),
        local(midpoint([now, other])),
      );
    } else
      setView((v) => ({
        ...v,
        x: v.x + now.x - was.x,
        y: v.y + now.y - was.y,
      }));
    pointers.current.set(event.pointerId, now);
  };
  const up = (event: PointerEvent) => pointers.current.delete(event.pointerId);
  const failing = names.filter((name) => seen[name]?.errors.length);
  const { x, y, scale: s } = view;
  return (
    <div class="canvas">
      {failing.length > 0 && (
        <p class="canvas-notice">
          <TriangleAlert />
          <span>
            {failing.length === 1
              ? "1 page failed to run"
              : `${failing.length} pages failed to run`}
          </span>
          {addToChat && (
            <Button
              variant="secondary"
              onClick={() =>
                addToChat(
                  failing
                    .map(
                      (name) =>
                        `\`${path(name)}\` threw: ${seen[name].errors.join("; ")}`,
                    )
                    .join("\n")
                    .slice(0, ERRORS_MAX),
                )
              }
            >
              Ask agent to fix
            </Button>
          )}
        </p>
      )}
      <div
        class="canvas-stage"
        ref={stage}
        onPointerDown={down}
        onPointerMove={move}
        onPointerUp={up}
        onPointerCancel={up}
      >
        <div
          class="canvas-plane"
          style={{ transform: `translate(${x}px, ${y}px) scale(${s})` }}
        >
          {placed.map((item) => {
            const title = seen[item.name]?.info?.title || item.name;
            return (
              <div
                key={item.name}
                class="canvas-page"
                data-failed={seen[item.name]?.errors.length ? true : undefined}
                style={{ left: `${item.x}px`, top: `${item.y}px` }}
              >
                <Frame
                  src={page(path(item.name))}
                  title={title}
                  width={item.w}
                  height={item.h}
                  live={false}
                  onMessage={heard(item.name)}
                />
                <Button
                  variant="quiet"
                  class="canvas-open"
                  aria-label={`Open ${title}`}
                  onClick={() => !moved.current && open(path(item.name))}
                />
              </div>
            );
          })}
        </div>
        {placed.map((item, at) => (
          <span
            key={item.name}
            class="canvas-name small"
            style={{
              left: `${x + item.x * s}px`,
              top: `${y + item.y * s}px`,
              // A name runs up to the next frame of its row; the last one's
              // as far as the canvas's names may (files.css).
              maxWidth:
                placed[at + 1]?.y === item.y
                  ? `${(placed[at + 1].x - item.x) * s}px`
                  : undefined,
            }}
          >
            {item.x === 0 && item.row ? `${item.row} · ` : ""}
            {seen[item.name]?.info?.title || item.name}
          </span>
        ))}
      </div>
      <ZoomControls
        percent={Math.round(s * 100)}
        out={() => center(1 / 1.25)}
        in={() => center(1.25)}
        fit={fit}
      />
    </div>
  );
}
