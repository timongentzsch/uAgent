import { useMemo, useState } from "preact/hooks";
import { Button } from "../../shared/ui.tsx";
import { ZoomSurface } from "../../shared/zoom-surface.tsx";
import type { ZoomView } from "../../shared/zoom.ts";
import { BoardFrame, type PageMessage } from "../../shared/page-frame.tsx";
import type { Board } from "./boards.ts";

// A design's boards on a plane you pan and zoom (ZoomSurface's plane), each
// a live page as the agent wrote it. The boards of a step (`02-` in
// `02-pricing--annual.html`) share a row; those without one form the first.
// A click opens a board.

// The plane: artboards at their real size, 80 px apart in a row and 240
// between rows (room for a name at any zoom), as Claude's canvas spaces them.
const GAP_X = 80;
const GAP_Y = 240;
const ZOOM: [number, number] = [0.05, 2];

export default function Plane({
  dir,
  boards,
  sized,
  page,
  heard,
  open,
}: {
  dir: string;
  boards: Board[];
  // Every board has said how large it is: the plane is fitted then.
  sized: boolean;
  // A file's preview address, with its version.
  page: (path: string) => string;
  heard: (path: string) => (message: PageMessage) => void;
  open: (path: string) => void;
}) {
  // Every board's place on the plane.
  const placed = useMemo(() => {
    let x = 0;
    let y = 0;
    let tall = 0;
    return boards.map((board, at) => {
      if (at && board.row !== boards[at - 1].row) {
        y += tall + GAP_Y;
        x = tall = 0;
      }
      const item = { ...board, x, y };
      x += board.w + GAP_X;
      tall = Math.max(tall, board.h);
      return item;
    });
  }, [boards]);
  const [view, setView] = useState<ZoomView>({ x: 0, y: 0, scale: 1 });
  // The whole design across the surface, under its first row of names.
  const fit = (width: number, height: number) => {
    if (!placed.length) return undefined;
    const w = Math.max(...placed.map((item) => item.x + item.w));
    const h = Math.max(...placed.map((item) => item.y + item.h));
    const scale = Math.min(1, (width - 48) / w, (height - 96) / h);
    return { x: (width - w * scale) / 2, y: 48, scale };
  };
  const { x, y, scale: s } = view;
  return (
    <div class="canvas">
      <ZoomSurface
        label="Canvas"
        range={ZOOM}
        fit={fit}
        fitKey={`${sized}:${dir}`}
        onView={setView}
        over={placed.map((item, at) => (
          <span
            key={item.path}
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
            {item.title}
          </span>
        ))}
      >
        {placed.map((item) => (
          <div
            key={item.path}
            class="canvas-page"
            data-failed={item.failed || undefined}
            style={{ left: `${item.x}px`, top: `${item.y}px` }}
          >
            <BoardFrame
              src={page(item.path)}
              title={item.title}
              w={item.w}
              h={item.h}
              onMessage={heard(item.path)}
            />
            <Button
              variant="quiet"
              class="cover"
              aria-label={`Open ${item.title}`}
              onClick={() => open(item.path)}
            />
          </div>
        ))}
      </ZoomSurface>
    </div>
  );
}
