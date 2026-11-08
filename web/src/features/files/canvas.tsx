import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { Minus, Plus, Scan, TriangleAlert } from "lucide-preact";
import { Button, IconButton } from "../../shared/ui.tsx";
import Frame, {
  type PageInfo,
  type PageMessage,
  pageSize,
} from "../../shared/page-frame.tsx";

// A folder's HTML files on a plane you pan and zoom, each a live page: a
// design as the agent wrote it. Files named <row>-<name>.html form a row per
// <row>; the rest form the first. Pinch, ⌘ or Ctrl and the wheel, or a
// trackpad pinch zoom at the pointer; a drag or the wheel pans. A click opens
// a page; a page that fails says so and offers its errors to the agent.

// The plane: artboards at their real size, 80 px apart in a row and 240
// between rows (room for a name at any zoom), as Claude's canvas spaces them.
const GAP_X = 80;
const GAP_Y = 240;
const ZOOM_MIN = 0.05;
const ZOOM_MAX = 2;
// A pointer that moved this far pans rather than opening the page under it.
const SLOP = 4;
interface View {
  x: number;
  y: number;
  s: number;
}
// What one message may carry of the pages' errors.
const ERRORS_MAX = 1800;

const rowOf = (name: string) =>
  name.includes("-") ? name.slice(0, name.indexOf("-")) : "";

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
  const [view, setView] = useState<View>({ x: 24, y: 48, s: 0.2 });
  const fit = () => {
    const box = stage.current;
    if (!box || !placed.length) return;
    const w = Math.max(...placed.map((item) => item.x + item.w));
    const h = Math.max(...placed.map((item) => item.y + item.h));
    const s = Math.min(
      1,
      (box.clientWidth - 48) / w,
      (box.clientHeight - 96) / h,
    );
    setView({ x: (box.clientWidth - w * s) / 2, y: 48, s });
  };
  // Fit once the pages have said how large they are, and for a new folder.
  const sized = names.every((name) => seen[name]?.info);
  // biome-ignore lint/correctness/useExhaustiveDependencies: on arrival
  useEffect(fit, [sized, dir]);
  // Zoom by `by` around a point of the stage, the point staying put.
  const zoomAt = (by: number, px: number, py: number) =>
    setView(({ x, y, s }) => {
      const next = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, s * by));
      return {
        x: px - ((px - x) * next) / s,
        y: py - ((py - y) * next) / s,
        s: next,
      };
    });
  const center = (by: number) => {
    const box = stage.current;
    if (box) zoomAt(by, box.clientWidth / 2, box.clientHeight / 2);
  };
  // One pointer pans, two pinch. A pointer that moved opens nothing.
  const pointers = useRef(new Map<number, { x: number; y: number }>());
  // The wheel pans; with ⌘ or Ctrl (a trackpad pinch sends Ctrl) it zooms.
  // biome-ignore lint/correctness/useExhaustiveDependencies: one listener, state by updater
  useEffect(() => {
    const box = stage.current;
    if (!box) return;
    const wheel = (event: WheelEvent) => {
      event.preventDefault();
      const at = box.getBoundingClientRect();
      if (event.ctrlKey || event.metaKey)
        zoomAt(
          Math.exp(-event.deltaY * 0.01),
          event.clientX - at.left,
          event.clientY - at.top,
        );
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
        zoomAt(next / scale, clientX - at.left, clientY - at.top);
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
    if (Math.hypot(now.x - was.x, now.y - was.y) > SLOP) moved.current = true;
    if (!moved.current) return;
    (event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);
    if (all.length === 2) {
      // What was under the fingers' midpoint stays under it, scaled by
      // how far apart they moved.
      const other = all.find((point) => point !== was)!;
      const at = stage.current!.getBoundingClientRect();
      const from = {
        x: (was.x + other.x) / 2 - at.left,
        y: (was.y + other.y) / 2 - at.top,
      };
      const to = {
        x: (now.x + other.x) / 2 - at.left,
        y: (now.y + other.y) / 2 - at.top,
      };
      const ratio =
        Math.hypot(now.x - other.x, now.y - other.y) /
        (Math.hypot(was.x - other.x, was.y - other.y) || 1);
      setView(({ x, y, s }) => {
        const next = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, s * ratio));
        return {
          x: to.x - ((from.x - x) * next) / s,
          y: to.y - ((from.y - y) * next) / s,
          s: next,
        };
      });
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
  const { x, y, s } = view;
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
        {placed.map((item) => (
          <span
            key={item.name}
            class="canvas-name small"
            style={{
              left: `${x + item.x * s}px`,
              top: `${y + item.y * s}px`,
              maxWidth: `${item.w * s}px`,
            }}
          >
            {item.x === 0 && item.row
              ? `${item.row.replace(/^\d+/, "")} · `
              : ""}
            {seen[item.name]?.info?.title || item.name}
          </span>
        ))}
      </div>
      <div class="float-toolbar canvas-zoom">
        <IconButton label="Zoom out" onClick={() => center(1 / 1.25)}>
          <Minus />
        </IconButton>
        <span class="small">{`${Math.round(s * 100)}%`}</span>
        <IconButton label="Zoom in" onClick={() => center(1.25)}>
          <Plus />
        </IconButton>
        <IconButton label="Fit" onClick={fit}>
          <Scan />
        </IconButton>
      </div>
    </div>
  );
}
