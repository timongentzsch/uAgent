import { nameOf } from "../../shared/file-types.ts";
import { ChevronLeft } from "lucide-preact";
import { useApi } from "../../state/api.ts";
import type {
  FileTarget,
  LibraryItem,
  LibraryPlace,
} from "../../shared/types.ts";
import { Button, LoadError, Spinner, Time } from "../../shared/ui.tsx";
import { bytes } from "../../shared/quantities.ts";
import { ARTIFACT, artifactSize } from "../../shared/artifact.ts";
import { PageThumb, usePreviewBase } from "../../shared/page-frame.tsx";

// An artifact or a shared file, as the Library shows one: its pages
// small, and the way to where it is worked on.

const KIND = { ...ARTIFACT, file: "Shared file" };
// A document and a sheet are one file; the others are their folder.
const OWN = new Set(["doc", "sheet"]);
const OPEN: Record<string, string> = {
  design: "Open canvas",
  deck: "Open deck",
  doc: "Open document",
  sheet: "Open sheet",
};

function Thumbnails({ session, pages }: { session: string; pages: string[] }) {
  const preview = usePreviewBase(session);
  if (preview.error)
    return <LoadError error={preview.error} retry={preview.retry} />;
  if (!preview.value) return <Spinner label="Drawing pages…" />;
  return (
    <div class="library-pages">
      {pages.map((page) => (
        <figure key={page}>
          <PageThumb base={preview.value!.base} path={page} />
          <figcaption class="small muted">{nameOf(page)}</figcaption>
        </figure>
      ))}
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
  const { routes } = useApi();
  const kind = item.kind as keyof typeof KIND;
  const file = kind === "file";
  const size = !file && item.count ? artifactSize(kind, item.count) : "";
  // A design system's book, which the agent writes beside its tokens.
  const book =
    kind === "system"
      ? item.pages?.find((page) => /(^|\/)book\.html$/.test(page))
      : undefined;
  const asset =
    file && item.session ? routes.asset(item.session, item.key) : "";
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
        {size && ` · ${size}`}
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
            onClick={() =>
              open({
                path: book ?? (OWN.has(kind) ? item.path : `${item.path}/`),
              })
            }
          >
            {OPEN[kind] ?? (book ? "Open its brand book" : "Open its folder")}
          </Button>
        )}
      </div>
      {!file && place?.session && item.pages?.length ? (
        <Thumbnails session={place.session} pages={item.pages} />
      ) : null}
    </div>
  );
}
