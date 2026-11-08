import { api, routes } from "../../state/api.ts";
import type { FileTarget } from "../../shared/types.ts";
import { Button } from "../../shared/ui.tsx";
import { HTML } from "../../shared/file-types.ts";
import { fileReference } from "../../shared/markdown.ts";
import { useResource } from "../../shared/use-resource.ts";
import { PageThumb, usePreviewBase } from "../../shared/page-frame.tsx";

// A design a reply names (`design/landing/`, `design/system/book.html`),
// shown under it as a card, as a shared file is: its first page live and
// small, and Open, which shows it in the Files pane.

// At most this many cards a reply shows.
const CARDS = 2;

// The folders and pages a reply names in code, in order.
export function designPaths(text: string) {
  const paths = [...text.matchAll(/`([^`\n]+)`/g)]
    .map(([, code]) => fileReference(code))
    .filter((reference) => reference && !reference.lines)
    .map((reference) => reference!.path)
    .filter((path) => path.endsWith("/") || HTML.test(path));
  return [...new Set(paths)].slice(0, CARDS);
}

function DesignCard({
  session,
  path,
  open,
}: {
  session: string;
  path: string;
  open: (target: FileTarget) => void;
}) {
  const dir = path.endsWith("/") ? path.slice(0, -1) : "";
  // A folder's pages, a design system's book first; a page is itself. What
  // is not there (a name the reply used loosely) gets no card.
  const folder = dir || path.slice(0, Math.max(0, path.lastIndexOf("/")));
  const pages = useResource(
    () =>
      api<{ entries: { name: string; dir: boolean }[] }>(
        routes.files(session, "list", folder),
      ).then(
        (listing) =>
          listing.entries
            .filter((entry) => !entry.dir && HTML.test(entry.name))
            .map((entry) => (folder ? `${folder}/${entry.name}` : entry.name))
            .filter((page) => dir || page === path)
            .sort((a, b) =>
              a.endsWith("/book.html")
                ? -1
                : b.endsWith("/book.html")
                  ? 1
                  : a < b
                    ? -1
                    : 1,
            ),
        () => [],
      ),
    [session, path],
  );
  const preview = usePreviewBase(session);
  const cover = pages.value?.[0];
  if (!cover || !preview.value) return null;
  const count = pages.value!.length;
  return (
    <figure class="node-file design-card">
      <Button
        variant="quiet"
        class="design-card-cover"
        aria-label={`Open ${path}`}
        onClick={() => open({ path })}
      >
        <PageThumb base={preview.value.base} path={cover} width={280} />
      </Button>
      <figcaption>
        <strong>{path}</strong>
        {dir && (
          <span class="muted">
            {cover.endsWith("/book.html")
              ? "Brand book"
              : `${count} ${count === 1 ? "page" : "pages"}`}
          </span>
        )}
        <Button variant="quiet" onClick={() => open({ path })}>
          Open
        </Button>
      </figcaption>
    </figure>
  );
}

export default function DesignCards({
  session,
  text,
  open,
}: {
  session: string;
  text: string;
  open: (target: FileTarget) => void;
}) {
  return (
    <>
      {designPaths(text).map((path) => (
        <DesignCard key={path} session={session} path={path} open={open} />
      ))}
    </>
  );
}
