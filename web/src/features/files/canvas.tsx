import { useMemo, useState } from "preact/hooks";
import {
  ChevronDown,
  FileOutput,
  Presentation,
  TriangleAlert,
} from "lucide-preact";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { saveLink } from "../../shared/share.ts";
import { Button } from "../../shared/ui.tsx";
import { ZoomSurface } from "../../shared/zoom-surface.tsx";
import type { ZoomView } from "../../shared/zoom.ts";
import Frame, {
  type PageInfo,
  type PageMessage,
  pageSize,
} from "../../shared/page-frame.tsx";

// A folder's HTML files on a plane you pan and zoom, each a live page: a
// design as the agent wrote it. A page's step (`02-` in
// `02-pricing--annual.html`) is its row; pages without one form the first.
// A deck, a folder whose every page is a slide, lies in slide order instead,
// numbered, and presents and prints from here.
// It zooms and pans as ZoomSurface's plane does. A click opens a page; a
// page that fails says so and offers its errors to the agent.

// The plane: artboards at their real size, 80 px apart in a row and 240
// between rows (room for a name at any zoom), as Claude's canvas spaces them.
const GAP_X = 80;
const GAP_Y = 240;
// A deck's slides to a row.
const DECK_COLUMNS = 4;
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
  present,
  deckPdf,
}: {
  dir: string;
  names: string[];
  // A file's preview address, with its version.
  page: (path: string) => string;
  open: (path: string) => void;
  addToChat?: (text: string) => void;
  // A deck: present it from its first slide, and its PDF at a slide size.
  present?: (path: string) => void;
  deckPdf?: (width: number, height: number) => string;
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
  const sizeOf = (name: string) => pageSize(seen[name]?.info);
  const deck =
    names.length > 0 &&
    names.every((name) => seen[name]?.info?.kind === "slide");
  const order = grouped.flatMap(([, files]) => files);
  // Every page's place on the plane.
  // biome-ignore lint/correctness/useExhaustiveDependencies: sizes come from seen
  const placed = useMemo(() => {
    if (deck)
      return order.map((name, at) => {
        const size = sizeOf(name);
        return {
          name,
          row: String(at + 1).padStart(2, "0"),
          x: (at % DECK_COLUMNS) * (size.w + GAP_X),
          y: Math.floor(at / DECK_COLUMNS) * (size.h + GAP_Y),
          ...size,
        };
      });
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
  }, [grouped, seen, deck]);
  const [view, setView] = useState<ZoomView>({ x: 0, y: 0, scale: 1 });
  // The whole design across the surface, under its first row of names.
  const fit = (width: number, height: number) => {
    if (!placed.length) return undefined;
    const w = Math.max(...placed.map((item) => item.x + item.w));
    const h = Math.max(...placed.map((item) => item.y + item.h));
    const scale = Math.min(1, (width - 48) / w, (height - 96) / h);
    return { x: (width - w * scale) / 2, y: 48, scale };
  };
  // Fitted once the pages have said how large they are, and for a new folder.
  const sized = names.every((name) => seen[name]?.info);
  const failing = names.filter((name) => seen[name]?.errors.length);
  const { x, y, scale: s } = view;
  return (
    <div class="canvas">
      {deck && (present || deckPdf) && (
        <div class="canvas-deck-tools">
          <span class="small muted">{order.length} slides</span>
          {deckPdf && (
            <Menu
              label="Export"
              trigger={
                <>
                  <FileOutput />
                  Export
                  <ChevronDown />
                </>
              }
            >
              <MenuItem
                onClick={() =>
                  saveLink(
                    `${deckPdf(placed[0].w, placed[0].h)}&download=1`,
                    `${dir.split("/").pop() || "deck"}.pdf`,
                  )
                }
              >
                PDF, a page per slide
              </MenuItem>
            </Menu>
          )}
          {present && (
            <Button variant="primary" onClick={() => present(path(order[0]))}>
              <Presentation />
              Present
            </Button>
          )}
        </div>
      )}
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
      <ZoomSurface
        label="Canvas"
        range={ZOOM}
        fit={fit}
        fitKey={`${sized}:${dir}`}
        onView={setView}
        over={placed.map((item, at) => (
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
            {(deck || item.x === 0) && item.row ? `${item.row} · ` : ""}
            {seen[item.name]?.info?.title || item.name}
          </span>
        ))}
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
                onClick={() => open(path(item.name))}
              />
            </div>
          );
        })}
      </ZoomSurface>
    </div>
  );
}
