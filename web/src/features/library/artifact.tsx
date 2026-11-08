import { useState } from "preact/hooks";
import { ChevronLeft } from "lucide-preact";
import { api, routes } from "../../state/api.ts";
import type {
  FileTarget,
  LibraryItem,
  LibraryPlace,
} from "../../shared/types.ts";
import { Button, LoadError, Spinner, Time } from "../../shared/ui.tsx";
import { bytes } from "../../shared/quantities.ts";
import { useResource } from "../../shared/use-resource.ts";
import Frame, { PAGE_WIDTH } from "../../shared/page-frame.tsx";

// A design, a design system or a shared file, as the Library shows one: its
// pages small, and the way to where it is worked on.

const KIND = { design: "Design", system: "Design system", file: "Shared file" };
// A thumbnail's width, in CSS pixels at the page's own size.
const THUMB = 240;

function Thumbnails({ session, pages }: { session: string; pages: string[] }) {
  const preview = useResource(
    () => api<{ base: string }>(routes.preview(session), {}),
    [session],
  );
  // Each page's artboard, once it says; a page is a desktop screen until then.
  const [sizes, setSizes] = useState<Record<string, { w: number; h: number }>>(
    {},
  );
  if (preview.error)
    return <LoadError error={preview.error} retry={preview.retry} />;
  if (!preview.value) return <Spinner label="Drawing pages…" />;
  return (
    <div class="library-pages">
      {pages.map((page) => {
        const size = sizes[page];
        const w = size?.w || PAGE_WIDTH;
        const h = size?.h || Math.round((PAGE_WIDTH * 10) / 16);
        return (
          <figure key={page}>
            <Frame
              src={`${preview.value!.base}${page.split("/").map(encodeURIComponent).join("/")}`}
              title={page}
              width={w}
              height={h}
              scale={THUMB / Math.max(w, h)}
              live={false}
              onMessage={(message) => {
                if (message.type === "ready" && message.artboard.w)
                  setSizes((all) => ({ ...all, [page]: message.artboard }));
              }}
            />
            <figcaption class="small muted">
              {page.split("/").at(-1)}
            </figcaption>
          </figure>
        );
      })}
    </div>
  );
}

export default function LibraryArtifact({
  item,
  place,
  close,
  open,
  choose,
}: {
  item: LibraryItem;
  place?: LibraryPlace;
  close: () => void;
  // Shows a path in the Files pane of the place's latest conversation.
  open: (target: FileTarget) => void;
  choose: (session: string) => void;
}) {
  const kind = item.kind as keyof typeof KIND;
  const file = kind === "file";
  const asset =
    file && item.session && item.id ? routes.asset(item.session, item.id) : "";
  return (
    <div class="library-document">
      <div class="editor-head">
        <Button variant="quiet" onClick={close}>
          <ChevronLeft />
          Back
        </Button>
        <strong>{item.name}</strong>
      </div>
      <p class="muted">
        {KIND[kind]} · {place?.name ?? item.place}
        {item.count
          ? ` · ${item.count} ${item.count === 1 ? "page" : "pages"}`
          : ""}
        {file && item.bytes !== undefined ? ` · ${bytes(item.bytes)}` : ""}
        {item.modified ? (
          <>
            {" · "}
            <Time value={item.modified} />
          </>
        ) : null}
      </p>
      <div class="library-actions">
        {file ? (
          <>
            <a
              class="file-button primary"
              href={asset}
              target="_blank"
              rel="noopener"
            >
              Open
            </a>
            <a
              class="file-button secondary"
              href={`${asset}?download=1`}
              download={item.name}
            >
              Download
            </a>
            {item.session && (
              <Button onClick={() => choose(item.session!)}>
                Open its conversation
              </Button>
            )}
          </>
        ) : (
          <Button
            variant="primary"
            onClick={() => open({ path: `${item.path}/` })}
          >
            {kind === "system" ? "Open its brand book" : "Open canvas"}
          </Button>
        )}
      </div>
      {!file && place?.session && item.pages?.length ? (
        <Thumbnails session={place.session} pages={item.pages} />
      ) : null}
    </div>
  );
}
