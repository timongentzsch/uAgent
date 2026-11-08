import { useEffect, useRef, useState } from "preact/hooks";
import { Printer } from "lucide-preact";
import { api, routes } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import { IconButton, LoadError, Spinner } from "../../shared/ui.tsx";
import { book } from "./book.ts";
import { readTokens } from "./tokens.ts";

// A design system's folder as its brand book: A4 pages built from its
// README, tokens.css and components (see book.ts), scaled to the pane and
// printed whole to PDF.

interface Listing {
  entries: { name: string; dir: boolean }[];
}
// An A4 page and the gap around it, in CSS pixels.
const SHEET = 794 + 48;
const read = (id: string, path: string) =>
  api<{ text: string }>(routes.files(id, "read", path)).then(
    (value) => value.text,
    () => "",
  );

export default function System({
  id,
  dir,
  turn,
  page,
  name,
}: {
  id: string;
  dir: string;
  // Re-read when a turn starts or ends, as the rest of the pane is.
  turn: unknown;
  page: (path: string) => string;
  // The folder's own name, for a book whose README is titled only so.
  name: string;
}) {
  const at = (name: string) => (dir ? `${dir}/${name}` : name);
  const html = useResource(async () => {
    const [css, readme, listing] = await Promise.all([
      read(id, at("tokens.css")),
      read(id, at("README.md")),
      api<Listing>(routes.files(id, "list", at("components"))).catch(() => ({
        entries: [],
      })),
    ]);
    const components = await Promise.all(
      listing.entries
        .filter((entry) => entry.dir)
        .map(async ({ name }) => {
          const path = at(`components/${name}/preview.html`);
          const [preview, about] = await Promise.all([
            read(id, path),
            read(id, at(`components/${name}/README.md`)),
          ]);
          const head = preview.split("\n", 1)[0];
          return {
            name,
            path,
            group: /group="([^"]*)"/.exec(head)?.[1] ?? "",
            height: Math.min(
              1200,
              Math.max(40, Number(/height=(\d+)/.exec(head)?.[1]) || 120),
            ),
            about:
              about
                .replace(/^#.*$/gm, "")
                .trim()
                .split(/\n\s*\n/)[0] ?? "",
          };
        }),
    );
    return book({
      name,
      readme,
      tokens: readTokens(css),
      components,
      sheet: page(at("tokens.css")),
      page,
      date: new Date().toLocaleDateString(undefined, { dateStyle: "long" }),
    });
  }, [id, dir, turn]);
  const box = useRef<HTMLDivElement>(null);
  const [room, setRoom] = useState({ w: SHEET, h: 0 });
  useEffect(() => {
    const observer = new ResizeObserver(([entry]) =>
      setRoom({ w: entry.contentRect.width, h: entry.contentRect.height }),
    );
    if (box.current) observer.observe(box.current);
    return () => observer.disconnect();
  }, []);
  const scale = Math.min(1, room.w / SHEET);
  // The book alone in a tab of this app, which prints it: every page a sheet.
  const print = () => {
    if (!html.value) return;
    const tab = window.open(
      URL.createObjectURL(new Blob([html.value], { type: "text/html" })),
    );
    tab?.addEventListener("load", () => setTimeout(() => tab.print(), 500));
  };
  return (
    <div class="system" ref={box}>
      {html.error ? (
        <LoadError error={html.error} retry={html.retry} />
      ) : !html.value ? (
        <Spinner label="Setting the book…" />
      ) : (
        <>
          <iframe
            class="system-book"
            title="Brand book"
            sandbox="allow-same-origin allow-modals"
            srcdoc={html.value}
            style={{
              // At least a sheet wide, else the pane: the book's ground
              // fills it, its pages centred.
              width: `${Math.max(SHEET, room.w / scale)}px`,
              height: `${room.h / scale}px`,
              transform: `scale(${scale})`,
            }}
          />
          <div class="float-toolbar system-print">
            <IconButton label="Print or save as PDF" onClick={print}>
              <Printer />
            </IconButton>
          </div>
        </>
      )}
    </div>
  );
}
