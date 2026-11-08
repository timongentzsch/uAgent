import { useMemo, useRef, useState } from "preact/hooks";
import { Minus, Plus, Scan, TriangleAlert } from "lucide-preact";
import { Button, IconButton } from "../../shared/ui.tsx";
import { stored, useStored } from "../../shared/storage.ts";
import { storageKeys } from "../../shared/storage-keys.ts";
import Frame, {
  PAGE_HEIGHT_MAX,
  PAGE_WIDTH,
  type PageInfo,
  type PageMessage,
} from "./frame.tsx";

// A folder's HTML files side by side, each a live page: a design as the
// agent wrote it. Files named <row>-<name>.html form a row per <row>; the
// rest form the first. A click opens a page; a page that fails says so and
// offers its errors to the agent.

const ZOOMS = [0.1, 0.15, 0.2, 0.25, 0.33, 0.5, 0.75, 1];
const zooms = stored<Record<string, number>>(storageKeys.canvas, {});
// What one message may carry of the pages' errors.
const ERRORS_MAX = 1800;

export const rowOf = (name: string) =>
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
  const [kept, setKept] = useStored(zooms);
  const zoom = kept[dir] ?? 0.2;
  const setZoom = (value: number) => setKept({ ...kept, [dir]: value });
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
            : {
                ...was,
                errors: [
                  ...was.errors,
                  `${message.message}${message.line ? ` (line ${message.line})` : ""}`,
                ],
              };
      return next ? { ...all, [name]: next } : all;
    });
  const box = useRef<HTMLDivElement>(null);
  const widthOf = (name: string) => seen[name]?.info?.artboard.w || PAGE_WIDTH;
  // The widest row across the canvas, its gaps left a tenth.
  const fit = () => {
    const widest = Math.max(
      ...grouped.map(([, files]) =>
        files.reduce((sum, name) => sum + widthOf(name), 0),
      ),
    );
    const room = (box.current?.clientWidth ?? 0) * 0.9;
    if (room && widest) setZoom(Math.min(1, room / widest));
  };
  const failing = names.filter((name) => seen[name]?.errors.length);
  const step = (by: number) => {
    const at = ZOOMS.findIndex((value) => value >= zoom);
    setZoom(ZOOMS[Math.min(ZOOMS.length - 1, Math.max(0, at + by))]);
  };
  return (
    <div class="canvas" ref={box}>
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
      <div class="canvas-rows">
        {grouped.map(([row, files]) => (
          <section key={row} class="canvas-row" aria-label={row || "Pages"}>
            {row && <h3 class="canvas-row-name">{row.replace(/^\d+/, "")}</h3>}
            <div class="canvas-strip">
              {files.map((name) => {
                const info = seen[name]?.info;
                const fixed = !!info?.artboard.w && !!info.artboard.h;
                const width = widthOf(name);
                const height = fixed
                  ? info!.artboard.h
                  : Math.min(PAGE_HEIGHT_MAX, info?.height || 900);
                const title = info?.title || name;
                return (
                  <figure
                    key={name}
                    class="canvas-page"
                    data-failed={seen[name]?.errors.length ? true : undefined}
                  >
                    <figcaption class="small">{title}</figcaption>
                    <div class="canvas-frame">
                      <Frame
                        src={page(path(name))}
                        title={title}
                        width={width}
                        height={height}
                        scale={zoom}
                        live={false}
                        onMessage={heard(name)}
                      />
                      <Button
                        variant="quiet"
                        class="canvas-open"
                        aria-label={`Open ${title}`}
                        onClick={() => open(path(name))}
                      />
                    </div>
                  </figure>
                );
              })}
            </div>
          </section>
        ))}
      </div>
      <div class="float-toolbar canvas-zoom">
        <IconButton label="Zoom out" onClick={() => step(-1)}>
          <Minus />
        </IconButton>
        <span class="small">{`${Math.round(zoom * 100)}%`}</span>
        <IconButton label="Zoom in" onClick={() => step(1)}>
          <Plus />
        </IconButton>
        <IconButton label="Fit" onClick={fit}>
          <Scan />
        </IconButton>
      </div>
    </div>
  );
}
