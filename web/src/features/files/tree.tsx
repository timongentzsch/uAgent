import { useEffect, useRef } from "preact/hooks";
import {
  ChevronRight,
  File,
  FileCode,
  FileText,
  Folder,
  FolderOpen,
  Image,
} from "lucide-preact";
import { useApi } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import { Button, LoadError } from "../../shared/ui.tsx";
import { HTML, IMAGE, MARKDOWN } from "../../shared/file-types.ts";
import { join, type Listing as Pages } from "./boards.ts";
import { folderView } from "./flow.ts";

// The conversation's folder as a tree, as VS Code's Explorer draws one. A
// folder that is a deck, a design or a design system offers Open on its row.

export interface Listing extends Pages {
  more: boolean;
}
interface TreeProps {
  session: string;
  // Read again when a turn starts or ends, or a file is written.
  version: string;
  expanded: Set<string>;
  // The folder in context (the open file's, or the last one clicked).
  dir: string;
  file: string;
  toggle: (path: string) => void;
  open: (path: string) => void;
  openFolder: (dir: string) => void;
  list?: (path: string, signal: AbortSignal) => Promise<Listing>;
}

const CODE = /\.(css|js|mjs|ts|tsx|json)$/i;
const icon = (name: string) =>
  HTML.test(name) || CODE.test(name) ? (
    <FileCode />
  ) : MARKDOWN.test(name) ? (
    <FileText />
  ) : IMAGE.test(name) ? (
    <Image />
  ) : (
    <File />
  );

function Entries({
  path,
  props,
  listing,
}: {
  path: string;
  props: TreeProps;
  listing: Listing;
}) {
  return (
    <>
      {listing.entries.map((entry) =>
        entry.dir ? (
          <FolderNode
            key={entry.name}
            path={join(path, entry.name)}
            name={entry.name}
            props={props}
          />
        ) : (
          <FileRow
            key={entry.name}
            path={join(path, entry.name)}
            name={entry.name}
            props={props}
          />
        ),
      )}
      {listing.more && (
        <li class="files-note small">
          Some entries may not be listed. Search to find the rest.
        </li>
      )}
    </>
  );
}

function FileRow({
  path,
  name,
  props,
}: {
  path: string;
  name: string;
  props: TreeProps;
}) {
  const row = useRef<HTMLButtonElement>(null);
  const current = path === props.file;
  // The open file's row comes into view, as VS Code reveals the active file.
  useEffect(() => {
    if (current) row.current?.scrollIntoView({ block: "nearest" });
  }, [current]);
  return (
    <li>
      <Button
        variant="quiet"
        class="entry tree-row"
        buttonRef={row}
        data-path={path}
        aria-current={current ? "true" : undefined}
        onClick={() => props.open(path)}
      >
        <span class="tree-twisty" />
        {icon(name)}
        <span class="files-name truncate">{name}</span>
      </Button>
    </li>
  );
}

function FolderNode({
  path,
  name,
  props,
}: {
  path: string;
  name: string;
  props: TreeProps;
}) {
  const { api, routes } = useApi();
  const open = props.expanded.has(path);
  const listing = useResource(
    (signal) =>
      open
        ? props.list
          ? props.list(path, signal)
          : api<Listing>(routes.files(props.session, "list", path), undefined, {
              signal,
            })
        : undefined,
    [props.session, path, open, props.version, props.list],
  );
  // What the folder opens as; a folder that is nothing but holds one page
  // has that page as a row and needs no Open.
  const opens = listing.value && folderView(path, listing.value);
  return (
    <li>
      <div class="tree-folder">
        <Button
          variant="quiet"
          class="entry tree-row"
          data-path={path}
          aria-expanded={open}
          aria-current={
            path === props.dir && !props.file ? "location" : undefined
          }
          onClick={() => props.toggle(path)}
        >
          <ChevronRight class="tree-twisty" />
          {open ? <FolderOpen /> : <Folder />}
          <span class="files-name truncate">{name}</span>
        </Button>
        {open && opens && (listing.value?.kind || opens.kind === "canvas") && (
          <Button
            variant="quiet"
            size="compact"
            onClick={() => props.openFolder(path)}
          >
            Open
          </Button>
        )}
      </div>
      {open &&
        (listing.error ? (
          <LoadError error={listing.error} retry={listing.retry} />
        ) : listing.value ? (
          <ul role="group">
            <Entries path={path} props={props} listing={listing.value} />
          </ul>
        ) : null)}
    </li>
  );
}

// The rows as the keyboard walks them: up and down through what is shown,
// right opens a folder or enters it, left closes it or goes to its parent.
function walk(event: KeyboardEvent, props: TreeProps) {
  const rows = [
    ...(event.currentTarget as HTMLElement).querySelectorAll<HTMLElement>(
      ".tree-row",
    ),
  ];
  const at = rows.indexOf(document.activeElement as HTMLElement);
  if (at < 0) return;
  const row = rows[at];
  const path = row.dataset.path!;
  const folder = row.hasAttribute("aria-expanded");
  const open = row.getAttribute("aria-expanded") === "true";
  const focus = (index: number) => rows[index]?.focus();
  if (event.key === "ArrowDown") focus(at + 1);
  else if (event.key === "ArrowUp") focus(at - 1);
  else if (event.key === "ArrowRight" && folder)
    open ? focus(at + 1) : props.toggle(path);
  else if (event.key === "ArrowLeft") {
    if (folder && open) props.toggle(path);
    else {
      const parent = path.slice(0, Math.max(0, path.lastIndexOf("/")));
      focus(rows.findIndex((each) => each.dataset.path === parent));
    }
  } else return;
  event.preventDefault();
}

export default function FileTree(
  props: TreeProps & { root: Listing; rootPath?: string },
) {
  return (
    <ul
      class="files-tree dense"
      onKeyDown={(event) => walk(event as unknown as KeyboardEvent, props)}
    >
      <Entries path={props.rootPath || ""} props={props} listing={props.root} />
    </ul>
  );
}
