import "./files.css";
import {
  useContext,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import {
  ChevronLeft,
  ChevronsDownUp,
  Download,
  File,
  Pencil,
  TriangleAlert,
  X,
} from "lucide-preact";
import { usePushDirection } from "../../shared/motion.ts";
import type { Session } from "../../shared/types.ts";
import { failure } from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import { useAction } from "../../shared/use-action.ts";
import {
  Button,
  EmptyState,
  IconButton,
  LoadError,
  Placeholder,
  Spinner,
  useDialog,
} from "../../shared/ui.tsx";
import { Input } from "../../shared/form-controls.tsx";
import { SuggestionList } from "../../shared/suggestion-list.tsx";
import { fuzzy } from "../../shared/fuzzy.ts";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { ImageViewer, ZoomImage } from "../../shared/attachments.tsx";
import { ResizeHandle } from "../../shared/side-pane.tsx";
import { MarkdownFile } from "../../shared/markdown-view.tsx";
import { diffLineClass, folderOf, formatStat } from "../../shared/display.ts";
import DiffView from "../../shared/diff-view.tsx";
import { shareOrDownload } from "../../shared/share.ts";
import { plural } from "../../shared/quantities.ts";
import { stored } from "../../shared/storage.ts";
import { storageKeys, hostStorageKey } from "../../shared/storage-keys.ts";
import {
  HTML,
  IMAGE,
  MARKDOWN,
  nameOf,
  paged,
  SHEET_FILE,
  VIDEO,
} from "../../shared/file-types.ts";
import { useHostContext } from "../../state/host-context.ts";
import { useSnapshots } from "../../state/snapshot-store.ts";
import type { Node } from "../../shared/types.ts";
import {
  boardsOf,
  carried,
  failures,
  hear,
  join,
  kindOf,
  type Move,
  rewritten,
  settled,
  type SeenPages,
} from "./boards.ts";
import Plane from "./plane.tsx";
import {
  backLabel,
  cite,
  dirOf,
  type Flow,
  follow,
  folderOfView,
  fromList,
  type Lines,
  lineRange,
  opened,
  pop,
  push,
  resolve,
  swap,
  type View,
} from "./flow.ts";
import FileTree, { type Listing } from "./tree.tsx";
import {
  type PageMessage,
  pageAddress,
  usePreviewBase,
} from "../../shared/page-frame.tsx";
import Sequence from "./sequence.tsx";
import { openPresenter } from "./present.ts";
import { applyEdit } from "./doc-edit.ts";
import { tableSaver } from "./sheet-edit.ts";

// The Files pane: a folder's tree, search and previews of text, images and
// Markdown, read from the host. Docked beside the chat or a sheet on a phone.

interface Found {
  paths: string[];
  more: boolean;
  // What was searched for, so an answer is matched to its question.
  query?: string;
}
interface Changed {
  changes: { path: string; state: "M" | "A" | "D" | "R" }[];
  more: boolean;
  repository: boolean;
}
interface FileText {
  text: string;
  bytes: number;
  more: boolean;
  // What a save is checked against; empty when the text was cut.
  revision?: string;
}

// A page that is text, which Source shows: not an Excel file's bytes.
const TEXT_PAGE = /\.(html?|csv)$/i;
// Search results shown at once; typing more narrows them.
// A folder as it is drawn before its entries arrive (see <Placeholder>).
const SAMPLE_LISTING: Listing = {
  entries: [
    { name: "src", dir: true },
    { name: "tests", dir: true },
    { name: "README.md", dir: false },
  ],
  more: false,
};
const SHOWN = 50;
// The folders open in the tree, per conversation folder, on this device.
const folderStore = (host: string) =>
  stored<Record<string, string[]>>(
    hostStorageKey(storageKeys.filesAt, host),
    {},
  );
// A path's folders from the top: `a/b` is `a` and `a/b`.
const ancestors = (dir: string) =>
  dir
    ? dir.split("/").map((_, at, parts) => parts.slice(0, at + 1).join("/"))
    : [];
const SEARCH_DELAY_MS = 150;

const WRITES = new Set(["Wrote", "Edited", "Deleted"]);
// The last file write that finished, as "<node>\n<path written>".
function lastWrite(nodes: readonly Node[] = []) {
  for (let at = nodes.length - 1; at >= 0; at--) {
    const node = nodes[at];
    if (node.kind === "call" && node.state === "ok" && node.headline) {
      if (WRITES.has(node.headline.verb))
        return `${node.id}\n${node.headline.target}`;
    }
  }
  return "";
}

// Where it is lives with the app (`flow`, see flow.ts), so it stays put while
// closed or moved between the sheet and the docked pane. What is open is read
// again when a turn ends: the turn may have changed the folder.
export default function Files({
  session,
  close,
  flow,
  setFlow,
  addToChat,
  pictures,
}: {
  session: Session;
  // Docked beside the chat; in a sheet, the sheet's own close.
  close?: () => void;
  flow: Flow;
  setFlow: (update: (flow: Flow) => Flow) => void;
  // Puts text in the conversation's draft; none where there is no draft.
  addToChat?: (text: string, keep?: boolean) => void;
  // Whether the agent's browser can draw a page as a picture.
  pictures?: boolean;
}) {
  const { routes, api, host } = useApi();
  const kept = useMemo(() => folderStore(host), [host]);
  // In a sheet, its header is this one: Close plays the sheet's exit, and
  // the title is where the sheet puts focus.
  const dialog = useDialog();
  const closeFiles = close ?? dialog?.close;
  const id = session.id;
  const turn = session.turn_active;
  const viewer = useContext(ImageViewer);
  const { view, back } = flow;
  const dir = folderOfView(view);
  const file =
    view.kind === "file" || view.kind === "sequence" ? view.path : "";
  const changing = view.kind === "file" && !!view.diff;
  const source = view.kind === "file" && !!view.source;
  const marked = view.kind === "file" ? view.lines : undefined;
  // From the tree, search or changes (the list itself): Back is the list.
  const show = (next: View) => setFlow(() => fromList(next));
  // A path clicked in a file or on a canvas: looked up, Back to here.
  const go = (path: string, lines?: Lines) =>
    setFlow((current) => follow(current, path, lines));
  const [expanded, setExpanded] = useState(() => {
    const saved = kept.read()[folderOf(session)];
    return new Set([...(Array.isArray(saved) ? saved : []), ...ancestors(dir)]);
  });
  // biome-ignore lint/correctness/useExhaustiveDependencies: the pane is keyed by its session
  useEffect(() => {
    kept.write({ ...kept.read(), [folderOf(session)]: [...expanded] });
  }, [expanded]);
  // The tree shows where the view is.
  useEffect(
    () => setExpanded((all) => new Set([...all, ...ancestors(dir)])),
    [dir],
  );
  const toggle = (path: string) =>
    setExpanded((all) => {
      const next = new Set(all);
      if (!next.delete(path)) next.add(path);
      return next;
    });
  const [tab, setTab] = useState<"files" | "changes">(
    changing ? "changes" : "files",
  );
  const [query, setQuery] = useState("");
  const [index, setIndex] = useState(0);
  // On a phone the list or the view shows: the view while something is in
  // it.
  const side =
    view.kind === "file" || view.kind === "outside" || view.kind === "resolve"
      ? "file"
      : "list";
  const moved = usePushDirection(side === "file");
  const { store } = useHostContext();
  const written = useSnapshots(store, (all) =>
    lastWrite(all[id]?.state?.view?.nodes),
  );
  // Each page's version: a write to it reloads its frame, and so does one
  // to a file pages share, unless the page has none (`shared` false).
  const [versions, setVersions] = useState<Record<string, number>>({});
  const preview = usePreviewBase(id);
  const pageUrl = (path: string, shared = true) =>
    preview.value &&
    `${pageAddress(preview.value.base, path)}?${SHEET_FILE.test(path) ? "sheet&" : ""}v=${versions[path] ?? 0}${shared ? `.${versions[""] ?? 0}` : ""}`;
  const url = (path: string) => routes.files(id, "raw", path);
  const list = (dir: string) => api<Listing>(routes.files(id, "list", dir));
  const readFile = (path: string) =>
    api<FileText>(routes.files(id, "read", path));
  // Tagged with its folder: another folder's listing stays shown until this
  // one answers, and is never taken for it.
  const listing = useResource(
    () => list(dir).then((value) => ({ ...value, dir })),
    [id, dir, turn, written],
  );
  const here = listing.value?.dir === dir ? listing.value : undefined;
  // The tree's top, read as the folder in context is.
  const root = useResource(() => list(""), [id, turn, written]);
  const search = query.trim();
  const found = useResource<Found | undefined>(
    (signal) =>
      search
        ? new Promise((done) => setTimeout(done, SEARCH_DELAY_MS)).then(() =>
            signal.aborted
              ? undefined
              : api<Found>(routes.files(id, "find", search)),
          )
        : undefined,
    [id, search],
  );
  const changed = useResource(
    () => api<Changed>(routes.files(id, "changes", "")),
    [id, turn],
  );
  const changes = changed.value?.changes ?? [];
  const deleted =
    changing && changes.some((c) => c.path === file && c.state === "D");
  const image = view.kind === "file" && IMAGE.test(file) && !changing;
  const video = view.kind === "file" && VIDEO.test(file) && !changing;
  const page = view.kind === "sequence";
  // The canvas, or one page in its sequence, across the whole pane.
  const stage = view.kind === "canvas" || page;
  const content = useResource(
    () =>
      view.kind === "file" && !image && !video && !changing
        ? readFile(file).then((value) => ({ ...value, path: file }))
        : undefined,
    [id, file, turn, changing, view.kind],
  );
  const diff = useResource(
    () =>
      changing
        ? api<FileText>(routes.files(id, "diff", file)).then((value) => {
            // Counted once as it arrives, not on every render.
            const kinds = value.text.split("\n").map(diffLineClass);
            const counts: [number, number] = [
              kinds.filter((kind) => kind === "diff-add").length,
              kinds.filter((kind) => kind === "diff-del").length,
            ];
            return { ...value, path: file, counts };
          })
        : undefined,
    [id, file, turn, changing],
  );
  const text = content.value?.path === file ? content.value : undefined;
  const diffText = diff.value?.path === file ? diff.value : undefined;
  const counts = diffText?.counts ?? [0, 0];
  const markdown = MARKDOWN.test(file) && !source;
  // Highlighted again only when the text changed, not on every read.
  const highlighted = useResource(
    () =>
      text && !markdown
        ? import("../../shared/highlight.ts").then(({ highlightLines }) => ({
            path: file,
            lines: highlightLines(text.text, file),
          }))
        : undefined,
    [text?.text, file, markdown],
  );
  const code = useMemo(() => {
    if (highlighted.value?.path !== file) return undefined;
    const [first, last] = marked ?? [0, -1];
    return highlighted.value.lines
      .map((line, at) => {
        const target = at + 1 >= first && at + 1 <= last ? " data-target" : "";
        return `<span class="line-number"${target}>${at + 1}</span><span class="line"${target}>${line}\n</span>`;
      })
      .join("");
  }, [highlighted.value, file, marked?.[0], marked?.[1]]);
  // The lines selected in the code, which Add to chat cites. Each line is
  // a number then its code, so a child's place gives its line.
  const [chosen, setChosen] = useState<[number, number]>();
  useEffect(() => {
    const lineOf = (node: globalThis.Node | null) => {
      const view = codeView.current;
      const element = node instanceof Element ? node : node?.parentElement;
      const row = element?.closest(".line, .line-number");
      if (!view || !row || row.parentElement !== view) return undefined;
      return Math.floor([...view.children].indexOf(row) / 2) + 1;
    };
    const update = () => {
      const selection = document.getSelection();
      const ends = selection?.isCollapsed
        ? []
        : [
            lineOf(selection?.anchorNode ?? null),
            lineOf(selection?.focusNode ?? null),
          ];
      setChosen(
        ends.length && ends.every(Boolean)
          ? [Math.min(ends[0]!, ends[1]!), Math.max(ends[0]!, ends[1]!)]
          : undefined,
      );
    };
    document.addEventListener("selectionchange", update);
    return () => document.removeEventListener("selectionchange", update);
  }, []);
  // The first marked line sits in the middle of the view, once per view.
  const codeView = useRef<HTMLPreElement>(null);
  const scrolled = useRef<View | undefined>(undefined);
  useLayoutEffect(() => {
    const first = codeView.current?.querySelector("[data-target]");
    if (!first || scrolled.current === view) return;
    scrolled.current = view;
    first.scrollIntoView({ block: "center" });
  }, [code, view]);

  const results = found.value
    ? fuzzy(found.value.paths, search, (path) => path).slice(0, SHOWN)
    : [];
  // A path being looked up settles once, on what it is (see flow.ts).
  // biome-ignore lint/correctness/useExhaustiveDependencies: once per lookup
  useEffect(() => {
    if (view.kind !== "resolve") return;
    let current = true;
    void resolve(flow, view, {
      list: (dir) =>
        list(dir).catch((error) => {
          if (failure(error).status === 404) return null;
          throw error;
        }),
      find: (name) =>
        api<Found>(routes.files(id, "find", nameOf(name))).then(
          (value) => value.paths,
        ),
    }).then(
      (settled) =>
        current &&
        setFlow((latest) => (latest.view === view ? settled : latest)),
      () =>
        current &&
        setFlow((latest) =>
          latest.view === view
            ? swap(latest, {
                kind: "list",
                dir: "",
                note: `${view.path} could not be looked up.`,
              })
            : latest,
        ),
    );
    return () => {
      current = false;
    };
  }, [view]);
  // A folder or page that went away (deleted, moved) leaves its view.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a failed or changed listing
  useEffect(() => {
    if ((view.kind === "list" || stage) && dir && listing.error)
      if (failure(listing.error).status === 404)
        setFlow((current) =>
          swap(current, {
            kind: "list",
            dir: "",
            note: `${dir} is not in this folder.`,
          }),
        );
    if (
      page &&
      here &&
      !here.entries.some((entry) => !entry.dir && entry.name === nameOf(file))
    )
      setFlow((current) =>
        swap(current, {
          kind: "list",
          dir,
          note: `${nameOf(file)} is not there any more.`,
        }),
      );
  }, [listing.error, here]);
  // A cited path with no change to show opens as the file.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a failed diff
  useEffect(() => {
    if (changing && diff.error && failure(diff.error).status === 404)
      setFlow((current) =>
        current.view.kind === "file"
          ? swap(current, { ...current.view, diff: false })
          : current,
      );
  }, [diff.error]);
  // What the folder's pages said they are, and from that the boards the
  // view shows: a design's on its canvas, or what the open page is part of.
  const [seen, setSeen] = useState<SeenPages>({});
  const heard = (path: string) => (message: PageMessage) =>
    setSeen((all) => hear(all, path, message));
  const kind = here && page ? kindOf(here, nameOf(file)) : "design";
  // A design's folders may be decks lying on its canvas: read with the
  // canvas, a dozen at most, and tagged with the folder they are of.
  const canvas = view.kind === "canvas";
  const folders = useResource(
    () =>
      canvas && here
        ? Promise.all(
            here.entries
              .filter((entry) => entry.dir)
              .slice(0, 12)
              .map(({ name }) =>
                list(join(dir, name)).then((inner) => [name, inner] as const),
              ),
          ).then((inner) => ({ dir, inner: Object.fromEntries(inner) }))
        : undefined,
    [id, canvas, here],
  );
  const boards = useMemo(
    () =>
      here && stage
        ? boardsOf(
            dir,
            here,
            seen,
            page ? nameOf(file) : undefined,
            folders.value?.dir === dir ? folders.value.inner : undefined,
          )
        : [],
    [here, stage, page, dir, seen, file, folders.value],
  );
  const failed = failures(boards, seen, kind);
  // A deck's slides renamed, copied or deleted by hand: the folder is read
  // again and the view follows to `show` in one step, so the slide is never
  // taken for gone. Refused (the agent changed the folder), nothing was moved.
  const arranging = useAction({ report: () => listing.retry() });
  const arrange = (moves: Move[], show: string) =>
    arranging.run(async () => {
      await api(routes.fileWrite(id), { path: dir, moves });
      listing.setValue({ ...(await list(dir)), dir });
      setSeen((all) => carried(all, dir, moves));
      setFlow((current) =>
        swap(
          current,
          show
            ? { kind: "sequence", path: join(dir, show) }
            : { kind: "list", dir },
        ),
      );
    });
  // biome-ignore lint/correctness/useExhaustiveDependencies: said of the folder it happened in
  useEffect(() => arranging.setError(null), [dir]);
  // A sheet's edits saved into its file. One that is not (the agent wrote
  // the file meanwhile) draws the sheet again from the file, and the pane
  // says why.
  const [unsaved, setUnsaved] = useState<unknown>();
  // biome-ignore lint/correctness/useExhaustiveDependencies: the pane is keyed by its session
  const tables = useMemo(
    () =>
      tableSaver({
        read: readFile,
        write: (held) => api<{ revision: string }>(routes.fileWrite(id), held),
        failed(error, path) {
          setUnsaved(error);
          setSeen((all) => rewritten(all, path));
          setVersions((all) => ({ ...all, [path]: (all[path] ?? 0) + 1 }));
        },
      }),
    [],
  );
  // Said of the file it happened in.
  useEffect(() => setUnsaved(undefined), [file]);
  // A write reloads the page it changed.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on each write
  useEffect(() => {
    const target = written.slice(written.indexOf("\n") + 1);
    if (!target) return;
    const cited = cite({ path: target }, folderOf(session)).view;
    const path = cited.kind === "resolve" ? cited.path : "";
    if (!path) return;
    // A page, or what pages share (a stylesheet, an image, the data a page
    // reads): all reload. An Excel or CSV file is both.
    const shared = !HTML.test(path);
    const keys = [...(paged(path) ? [path] : []), ...(shared ? [""] : [])];
    setVersions((all) => ({
      ...all,
      ...Object.fromEntries(keys.map((key) => [key, (all[key] ?? 0) + 1])),
    }));
    // An open sheet is drawn anew only by a write to itself: it has no
    // styles or scripts of its own, and a reload ends its undo.
    const alone = kind === "sheet" && path !== file;
    setSeen((all) => ({
      ...rewritten(all, shared ? "" : path),
      ...(alone && all[file] && { [file]: all[file] }),
    }));
    if (kind === "sheet" && path === file) tables.renew();
  }, [written]);
  const titled =
    view.kind === "canvas"
      ? dir
      : view.kind === "pick"
        ? view.name
        : view.kind === "resolve"
          ? view.path
          : file;
  const folderName = nameOf(folderOf(session)) || "Files";
  const fileError = content.error ? failure(content.error) : undefined;
  const above = back[back.length - 1];
  // The file itself, saved or shared.
  const download = {
    href: `${url(file)}&download=1`,
    download: nameOf(file),
    onClick: shareOrDownload,
  };
  // Paths to pick from, under the search field.
  const matches = (paths: string[], pick: (path: string) => void) => (
    <SuggestionList
      id="files-results"
      class="files-results"
      label="Matching files"
      prefix="files-result"
      items={paths}
      index={index}
      pick={pick}
      keyOf={(path) => path}
    >
      {(path) => (
        <>
          <span class="files-name truncate">{nameOf(path)}</span>
          <span class="files-dir">{dirOf(path)}</span>
        </>
      )}
    </SuggestionList>
  );
  // What the stage waits for: the pages' address, then the folder, which
  // says what a page is part of.
  const pending = !preview.value ? preview : !here ? listing : undefined;

  return (
    <section
      class="files"
      aria-label="Files"
      data-showing={side}
      // A click in the pane leaves focus in it (Safari focuses no button),
      // so the keys of what it shows are its own.
      tabIndex={-1}
    >
      <header class="files-head">
        {above && (
          <IconButton
            // Beside the list on a wide pane, a file needs no way back to
            // it; everything else covers the list.
            class={
              view.kind === "file" && above.kind === "list"
                ? "files-back"
                : "files-back files-back-canvas"
            }
            label={backLabel(above)}
            onClick={() => setFlow(pop)}
          >
            <ChevronLeft />
          </IconButton>
        )}
        <h2
          class="files-title"
          title={titled || undefined}
          ref={dialog?.heading}
          tabIndex={dialog ? -1 : undefined}
        >
          {titled ? (
            <>
              <span class="files-dir">
                {dirOf(titled) && `${dirOf(titled)}/`}
              </span>
              <span class="files-name truncate">{nameOf(titled)}</span>
            </>
          ) : (
            folderName
          )}
        </h2>
        {diffText && formatStat(counts) && (
          <span class="files-dir small">{formatStat(counts)}</span>
        )}

        {file && !changing && (MARKDOWN.test(file) || TEXT_PAGE.test(file)) && (
          <Button
            variant="secondary"
            aria-pressed={source}
            onClick={() =>
              setFlow((current) =>
                swap(
                  current,
                  page
                    ? { kind: "file", path: file, source: true }
                    : source && paged(file)
                      ? { kind: "sequence", path: file }
                      : { kind: "file", path: file, source: !source },
                ),
              )
            }
          >
            Source
          </Button>
        )}
        {image && (
          <Button
            variant="secondary"
            onClick={() =>
              viewer({ src: url(file), name: nameOf(file), edit: true })
            }
          >
            <Pencil />
            Annotate
          </Button>
        )}
        {addToChat && chosen && file && !changing && (
          <Button
            variant="secondary"
            // Pressing it must not drop the selection it cites.
            onMouseDown={(event) => event.preventDefault()}
            onClick={() =>
              addToChat(
                `\`${file}:${chosen[0]}${chosen[1] > chosen[0] ? `-${chosen[1]}` : ""}\``,
              )
            }
          >
            Add to chat
          </Button>
        )}
        {file && !deleted && (
          <a
            class="file-button quiet icon-button"
            {...download}
            aria-label={`Download ${nameOf(file)}`}
            title={`Download ${nameOf(file)}`}
          >
            <Download />
          </a>
        )}
        {closeFiles && (
          <IconButton label="Close files" onClick={closeFiles}>
            <X />
          </IconButton>
        )}
      </header>
      {stage && (
        <div class="files-stage">
          {arranging.error != null && (
            <p class="page-notice" role="status">
              <TriangleAlert />
              <span>
                {[404, 409].includes(failure(arranging.error).status ?? 0)
                  ? "The deck's folder changed meanwhile, so nothing was moved."
                  : failure(arranging.error).message}
              </span>
            </p>
          )}
          {unsaved != null && (
            <p class="page-notice" role="status">
              <TriangleAlert />
              <span>
                {failure(unsaved).status === 409
                  ? "The last change was not saved: the file changed meanwhile. The sheet is shown as the file is now."
                  : `The last change was not saved: ${failure(unsaved).message}`}
              </span>
            </p>
          )}
          {failed.count > 0 && (
            <p class="page-notice">
              <TriangleAlert />
              <span>
                {kind === "sheet"
                  ? `${plural(failed.count, "cell")} in error`
                  : `${plural(failed.count, "page")} failed to run`}
              </span>
              {addToChat && (
                <Button
                  variant="secondary"
                  onClick={() => addToChat(failed.text)}
                >
                  Ask agent to fix
                </Button>
              )}
            </p>
          )}
          {pending ? (
            pending.error ? (
              <LoadError error={pending.error} retry={pending.retry} />
            ) : (
              <Spinner label="Opening the pages…" />
            )
          ) : page ? (
            <Sequence
              // A sheet's viewer keeps nothing of another page's.
              key={kind}
              path={file}
              kind={kind}
              boards={boards}
              page={(path) => pageUrl(path, kind !== "sheet")!}
              heard={heard}
              go={(path) =>
                setFlow((current) => swap(current, { kind: "sequence", path }))
              }
              addToChat={addToChat}
              base={preview.value!.base}
              drawn={
                pictures ? (as, path) => routes.files(id, as, path) : undefined
              }
              presenter={(slides, at) =>
                openPresenter({ session: id, slides, at })
              }
              save={async (edit) => {
                if ("table" in edit) {
                  await tables.save(file, edit);
                  return setUnsaved(undefined);
                }
                const read = await readFile(file);
                const text = read.revision ? applyEdit(read.text, edit) : null;
                if (text === null)
                  throw new Error(
                    "The document changed since it was shown. Reopen it to edit.",
                  );
                await api(routes.fileWrite(id), {
                  path: file,
                  text,
                  revision: read.revision,
                });
              }}
              saveAs={async (text) => {
                const path = join(
                  dir,
                  nameOf(file).replace(/\.[^.]+$/, ".html"),
                );
                await api(routes.fileWrite(id), { path, text, create: true });
                listing.setValue({ ...(await list(dir)), dir });
                setFlow((current) => push(current, { kind: "sequence", path }));
              }}
              arrange={kind === "deck" ? arrange : undefined}
              arranging={arranging.busy}
              view={viewer}
              read={(path) => readFile(path).then((value) => value.text)}
            />
          ) : (
            <Plane
              dir={dir}
              boards={boards}
              sized={boards.every((board) => settled(board, seen))}
              page={(path) => pageUrl(path)!}
              heard={heard}
              open={(path) => setFlow((current) => push(current, opened(path)))}
            />
          )}
        </div>
      )}
      <div
        class="files-columns"
        hidden={stage || undefined}
        data-direction={moved === "back" ? "back" : undefined}
      >
        <nav
          class={moved ? "files-list push" : "files-list"}
          aria-label="Folder"
        >
          <ResizeHandle
            pane="files-list"
            label="Resize the file list"
            edge="end"
          />
          {changed.value?.repository && (
            <div class="files-modes">
              <Button
                variant="secondary"
                aria-pressed={tab === "files"}
                onClick={() => setTab("files")}
              >
                Files
              </Button>
              <Button
                variant="secondary"
                aria-pressed={tab === "changes"}
                onClick={() => setTab("changes")}
              >
                {`Changes ${changes.length}`}
              </Button>
            </div>
          )}
          <div class="files-search-row">
            <Input
              type="search"
              aria-label="Search files"
              placeholder="Search files…"
              role="combobox"
              aria-expanded={!!search}
              aria-controls="files-results"
              aria-autocomplete="list"
              aria-activedescendant={
                results.length ? `files-result-${index}` : undefined
              }
              value={query}
              onInput={(event) => {
                setQuery(event.currentTarget.value);
                setIndex(0);
              }}
              onKeyDown={(event) => {
                if (event.isComposing || !plainKey(event)) return;
                if (event.key === "Escape" && query) setQuery("");
                else if (
                  (event.key === "ArrowDown" || event.key === "ArrowUp") &&
                  results.length
                )
                  setIndex(nextIndex(index, event.key, results.length));
                else if (event.key === "Enter" && results[index])
                  show(opened(results[index]));
                else return;
                event.preventDefault();
                event.stopPropagation();
              }}
            />
            {tab === "files" && (
              <IconButton
                label="Collapse all"
                onClick={() => setExpanded(new Set())}
              >
                <ChevronsDownUp />
              </IconButton>
            )}
          </div>
          {view.kind === "list" && view.note && (
            <p class="files-note small" role="status">
              {view.note}
            </p>
          )}
          {view.kind === "pick" && !search ? (
            <>
              <p class="files-note small">
                {`${view.paths.length} files are named ${nameOf(view.name)}.`}
                {view.lines
                  ? ` Pick one to open it at line ${view.lines[0]}.`
                  : " Pick one to open it."}
              </p>
              {matches(view.paths, (path) => go(path, view.lines))}
            </>
          ) : search ? (
            <>
              {matches(results, (path) => show(opened(path)))}
              {found.error ? (
                <LoadError error={found.error} retry={found.retry} />
              ) : found.value && !results.length ? (
                <EmptyState>No file name matches.</EmptyState>
              ) : (
                (found.value?.more ||
                  (found.value && found.value.paths.length > SHOWN)) && (
                  <p class="files-note small">
                    Some entries may not be listed. Keep typing to narrow.
                  </p>
                )
              )}
            </>
          ) : tab === "changes" ? (
            <>
              {changes.length ? (
                <ul class="files-tree dense">
                  {changes.map((change) => (
                    <li key={change.path}>
                      <Button
                        variant="quiet"
                        class="entry tree-row"
                        aria-current={change.path === file ? "true" : undefined}
                        onClick={() =>
                          show({
                            kind: "file",
                            path: change.path,
                            diff: true,
                          })
                        }
                      >
                        <span class="files-state" data-state={change.state}>
                          {change.state}
                        </span>
                        <span class="files-name truncate">{change.path}</span>
                      </Button>
                    </li>
                  ))}
                </ul>
              ) : (
                <EmptyState>No uncommitted changes</EmptyState>
              )}
              {changed.value?.more && (
                <p class="files-note small">Some changes may not be listed.</p>
              )}
            </>
          ) : (
            <>
              {root.error ? (
                <LoadError error={root.error} retry={root.retry} />
              ) : root.value && !root.value.entries.length ? (
                <EmptyState>This folder is empty.</EmptyState>
              ) : (
                <Placeholder label="Loading files…" when={!root.value}>
                  <FileTree
                    root={root.value ?? SAMPLE_LISTING}
                    session={id}
                    version={`${turn}${written}`}
                    expanded={expanded}
                    dir={dir}
                    file={file}
                    toggle={toggle}
                    open={(path) => show(opened(path))}
                    openFolder={(path) =>
                      setFlow(() =>
                        cite({ path: `${path}/` }, folderOf(session)),
                      )
                    }
                  />
                </Placeholder>
              )}
            </>
          )}
        </nav>
        <div
          class={moved ? "files-view push" : "files-view"}
          // Delegated: the rendered markdown's file links and images.
          onClick={(event) => {
            const target = event.target as HTMLElement;
            const link = target.closest<HTMLAnchorElement>("a[data-file]");
            if (link && !(event.metaKey || event.ctrlKey)) {
              event.preventDefault();
              go(link.dataset.file!);
            }
            const reference = target.closest<HTMLElement>(".file-ref");
            if (reference)
              go(reference.dataset.file!, lineRange(reference.dataset.lines));
            if (
              target instanceof HTMLImageElement &&
              target.closest(".markdown")
            )
              viewer({ src: target.src, name: target.alt || nameOf(file) });
          }}
        >
          {view.kind === "outside" ? (
            <EmptyState>
              {`${view.path} is outside this conversation's folder, so it can't be shown here.`}
            </EmptyState>
          ) : view.kind === "resolve" ? (
            <Spinner label={`Looking for ${nameOf(view.path)}…`} />
          ) : !file ? (
            <EmptyState>Choose a file to read it here.</EmptyState>
          ) : changing ? (
            diff.error ? (
              <LoadError error={diff.error} retry={diff.retry} />
            ) : !diffText ? (
              <Spinner label="Reading the changes…" />
            ) : (
              <>
                <DiffView text={diffText.text} counts={counts} />
                {diffText.more && (
                  <p class="files-note files-cut">
                    Showing the first part of this change.
                  </p>
                )}
              </>
            )
          ) : image ? (
            <div class="files-image">
              <ZoomImage key={url(file)} src={url(file)} name={nameOf(file)} />
            </div>
          ) : video ? (
            // From the preview address: the file route only downloads one.
            <div class="files-image">
              <video controls src={pageUrl(file)} />
            </div>
          ) : fileError ? (
            fileError.status === 415 ? (
              <div class="files-binary">
                <File />
                <p>{nameOf(file)} is not text, so it can't be shown here.</p>
                <a {...download}>Download</a>
              </div>
            ) : (
              <LoadError error={content.error} retry={content.retry} />
            )
          ) : !text ? (
            <Spinner label="Reading the file…" />
          ) : (
            <>
              {markdown ? (
                <MarkdownFile text={text.text} base={dirOf(file)} url={url} />
              ) : code ? (
                <pre
                  class="code-view"
                  ref={codeView}
                  // The highlighter's own spans, escaped by it.
                  dangerouslySetInnerHTML={{ __html: code }}
                />
              ) : (
                <Spinner label="Highlighting…" />
              )}
              {text.more && (
                <p class="files-note files-cut">
                  Showing the first part of this file.{" "}
                  <a {...download}>Download it</a> for the rest.
                </p>
              )}
            </>
          )}
        </div>
      </div>
    </section>
  );
}
