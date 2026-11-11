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
import { PageThumb, usePreviewBase } from "../../shared/page-frame.tsx";

// A design, a design system or a shared file, as the Library shows one: its
// pages small, and the way to where it is worked on.

const KIND = {
  design: "Design",
  deck: "Deck",
  doc: "Doc",
  system: "Design system",
  file: "Shared file",
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
  const unit = kind === "deck" ? "slide" : "page";
  const file = kind === "file";
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
        {item.count && kind !== "doc"
          ? ` · ${item.count} ${unit}${item.count === 1 ? "" : "s"}`
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
            onClick={() =>
              open({
                path: book ?? (kind === "doc" ? item.path : `${item.path}/`),
              })
            }
          >
            {kind === "design"
              ? "Open canvas"
              : kind === "deck"
                ? "Open deck"
                : kind === "doc"
                  ? "Open document"
                  : book
                    ? "Open its brand book"
                    : "Open its folder"}
          </Button>
        )}
      </div>
      {!file && place?.session && item.pages?.length ? (
        <Thumbnails session={place.session} pages={item.pages} />
      ) : null}
    </div>
  );
}
