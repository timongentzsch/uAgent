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
  X,
} from "lucide-preact";
import { usePushDirection } from "../../shared/motion.ts";
import type { Session } from "../../shared/types.ts";
import { failure } from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
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
import { stored } from "../../shared/storage.ts";
import { storageKeys, hostStorageKey } from "../../shared/storage-keys.ts";
import { HTML, IMAGE, MARKDOWN, nameOf } from "../../shared/file-types.ts";
import { useHostContext } from "../../state/host-context.ts";
import { useSnapshots } from "../../state/snapshot-store.ts";
import type { Node } from "../../shared/types.ts";
import Canvas, { rows } from "./canvas.tsx";
import {
  backLabel,
  cite,
  dirOf,
  type Flow,
  follow,
  folderOfView,
  type Lines,
  lineRange,
  pop,
  push,
  resolve,
  swap,
  type View,
} from "./flow.ts";
import FileTree from "./tree.tsx";
import { pageAddress, usePreviewBase } from "../../shared/page-frame.tsx";
import OpenPage from "./page.tsx";
import { openPresenter } from "./present.ts";
import { applyEdit } from "./doc-edit.ts";

// The Files pane: a folder's tree, search and previews of text, images and
// Markdown, read from the host. Docked beside the chat or a sheet on a phone.

interface Listing {
  entries: { name: string; dir: boolean; size: number }[];
  more: boolean;
}
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

// Search results shown at once; typing more narrows them.
// A folder as it is drawn before its entries arrive (see <Placeholder>).
const SAMPLE_LISTING: Listing = {
  entries: [
    { name: "src", dir: true, size: 0 },
    { name: "tests", dir: true, size: 0 },
    { name: "README.md", dir: false, size: 0 },
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
// Typing pauses this long before the folder is searched.
const SEARCH_DELAY_MS = 150;
const join = (dir: string, name: string) => (dir ? `${dir}/${name}` : name);

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

// The conversation's folder, read only: a list with search beside a viewer
// for text and code, rendered markdown and images, pages and their canvas.
// Where it is lives with the app (`flow`, see flow.ts), so it stays put
// while closed or moved between the sheet and the docked pane. A turn that
// ends may have changed the folder, so what is open is read again then.
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
  // The folder the view shows or sits in, and what it has open.
  const dir = folderOfView(view);
  const file = view.kind === "file" || view.kind === "page" ? view.path : "";
  const changing = view.kind === "file" && !!view.diff;
  const source = view.kind === "file" && !!view.source;
  const marked = view.kind === "file" ? view.lines : undefined;
  // From the tree, search or changes (the list itself): Back is the list.
  const fromList = (next: View) =>
    setFlow(() => ({
      view: next,
      back: [{ kind: "list", dir: folderOfView(next) }],
    }));
  // A path clicked in a file or on a canvas: looked up, Back to here.
  const go = (path: string, lines?: Lines) =>
    setFlow((current) => follow(current, path, lines));
  // What a path from the list opens: a page across the pane, else the file.
  const opened = (path: string, lines?: Lines): View =>
    HTML.test(path) && !lines
      ? { kind: "page", path }
      : {
          kind: "file",
          path,
          lines,
          source: !!lines && (MARKDOWN.test(path) || HTML.test(path)),
        };
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
  // The list column's own modes: the tree or the changes, and a search.
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
  // Each page's version: a write to it reloads its frame.
  const [versions, setVersions] = useState<Record<string, number>>({});
  const preview = usePreviewBase(id);
  const pageUrl = (path: string) =>
    preview.value &&
    `${pageAddress(preview.value.base, path)}?v=${versions[path] ?? 0}.${versions[""] ?? 0}`;
  const url = (path: string) => routes.files(id, "raw", path);
  // Tagged with its folder: another folder's listing stays shown until this
  // one answers, and is never taken for it.
  const listing = useResource(
    () =>
      api<Listing>(routes.files(id, "list", dir)).then((value) => ({
        ...value,
        dir,
      })),
    [id, dir, turn, written],
  );
  const here = listing.value?.dir === dir ? listing.value : undefined;
  // The tree's top, read as the folder in context is.
  const root = useResource(
    () => api<Listing>(routes.files(id, "list", "")),
    [id, turn, written],
  );
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
  const page = view.kind === "page";
  // The canvas, or one page, across the whole pane.
  const stage = view.kind === "canvas" || page;
  const content = useResource(
    () =>
      view.kind === "file" && !image && !changing
        ? api<FileText>(routes.files(id, "read", file)).then((value) => ({
            ...value,
            path: file,
          }))
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
        api<Listing>(routes.files(id, "list", dir)).catch((error) => {
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
    if (view.kind === "list" && dir && listing.error)
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
  // A cited change that is no longer one opens as the file.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a failed diff
  useEffect(() => {
    if (changing && diff.error && failure(diff.error).status === 404)
      setFlow((current) =>
        current.view.kind === "file"
          ? swap(current, { ...current.view, diff: false })
          : current,
      );
  }, [diff.error]);
  const pages = (here?.entries ?? [])
    .filter((entry) => !entry.dir && HTML.test(entry.name))
    .map((entry) => entry.name);
  // A write reloads the page it changed.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on each write
  useEffect(() => {
    const target = written.slice(written.indexOf("\n") + 1);
    if (!target) return;
    const cited = cite({ path: target }, folderOf(session)).view;
    const path = cited.kind === "resolve" ? cited.path : "";
    // A page, or what pages share (a stylesheet, an image): all reload.
    const key = path && HTML.test(path) ? path : "";
    if (path) setVersions((all) => ({ ...all, [key]: (all[key] ?? 0) + 1 }));
  }, [written]);
  const titled =
    view.kind === "canvas"
      ? dir
      : view.kind === "resolve" || view.kind === "pick"
        ? view.kind === "pick"
          ? view.name
          : view.path
        : file;
  const folderName = nameOf(folderOf(session)) || "Files";
  const fileError = content.error ? failure(content.error) : undefined;
  const above = back[back.length - 1];

  return (
    <section class="files" aria-label="Files" data-showing={side}>
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

        {file && !changing && (MARKDOWN.test(file) || HTML.test(file)) && (
          <Button
            variant="secondary"
            aria-pressed={source}
            onClick={() =>
              setFlow((current) =>
                swap(
                  current,
                  page
                    ? { kind: "file", path: file, source: true }
                    : source && HTML.test(file)
                      ? { kind: "page", path: file }
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
            href={`${url(file)}&download=1`}
            download={nameOf(file)}
            onClick={shareOrDownload}
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
          {!preview.value ? (
            preview.error ? (
              <LoadError error={preview.error} retry={preview.retry} />
            ) : (
              <Spinner label="Opening the pages…" />
            )
          ) : page ? (
            <OpenPage
              path={file}
              src={pageUrl(file)!}
              siblings={rows(pages).flatMap(([, names]) =>
                names.map((name) => join(dir, name)),
              )}
              go={(path) =>
                setFlow((current) => swap(current, { kind: "page", path }))
              }
              addToChat={addToChat}
              base={preview.value.base}
              picture={
                pictures
                  ? (w, h, scale) =>
                      `${routes.files(id, "png", file)}&w=${w}&h=${h}&scale=${scale}`
                  : undefined
              }
              pdf={pictures ? routes.files(id, "pdf", file) : undefined}
              presenter={(slides, at) =>
                openPresenter({ session: id, slides, at })
              }
              save={async (edit) => {
                const read = await api<FileText>(
                  routes.files(id, "read", file),
                );
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
              presentAtOpen={view.kind === "page" && !!view.present}
              presented={() =>
                setFlow((current) =>
                  current.view.kind === "page"
                    ? swap(current, { ...current.view, present: false })
                    : current,
                )
              }
              view={viewer}
              read={(path) =>
                api<FileText>(routes.files(id, "read", path)).then(
                  (value) => value.text,
                )
              }
            />
          ) : !here ? (
            <Spinner label="Opening the pages…" />
          ) : (
            <Canvas
              dir={dir}
              names={pages}
              page={(path) => pageUrl(path)!}
              open={(path) => setFlow((current) => push(current, opened(path)))}
              addToChat={addToChat}
              present={(path) =>
                setFlow((current) =>
                  push(current, { kind: "page", path, present: true }),
                )
              }
              deckPdf={
                pictures
                  ? (w, h) =>
                      `${routes.files(id, "pdf", `${dir}/`)}&w=${w}&h=${h}`
                  : undefined
              }
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
              class="files-search"
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
                  fromList(opened(results[index]));
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
              <SuggestionList
                id="files-results"
                class="files-results"
                label="Matching files"
                prefix="files-result"
                items={view.paths}
                index={index}
                pick={(path) =>
                  setFlow((current) => follow(current, path, view.lines))
                }
                keyOf={(path) => path}
              >
                {(path) => (
                  <>
                    <span class="files-name truncate">{nameOf(path)}</span>
                    <span class="files-dir">{dirOf(path)}</span>
                  </>
                )}
              </SuggestionList>
            </>
          ) : search ? (
            <>
              <SuggestionList
                id="files-results"
                class="files-results"
                label="Matching files"
                prefix="files-result"
                items={results}
                index={index}
                pick={(path) => fromList(opened(path))}
                keyOf={(path) => path}
              >
                {(path) => (
                  <>
                    <span class="files-name truncate">{nameOf(path)}</span>
                    <span class="files-dir">{dirOf(path)}</span>
                  </>
                )}
              </SuggestionList>
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
                          fromList({
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
                    open={(path) => fromList(opened(path))}
                    canvas={(path) =>
                      setFlow(() => ({
                        view: { kind: "canvas", dir: path },
                        back: [{ kind: "list", dir: path }],
                      }))
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
          ) : fileError ? (
            fileError.status === 415 ? (
              <div class="files-binary">
                <File />
                <p>{nameOf(file)} is not text, so it can't be shown here.</p>
                <a
                  href={`${url(file)}&download=1`}
                  download={nameOf(file)}
                  onClick={shareOrDownload}
                >
                  Download
                </a>
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
                  <a
                    href={`${url(file)}&download=1`}
                    download={nameOf(file)}
                    onClick={shareOrDownload}
                  >
                    Download it
                  </a>{" "}
                  for the rest.
                </p>
              )}
            </>
          )}
        </div>
      </div>
    </section>
  );
}
