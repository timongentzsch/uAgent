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
  Download,
  File,
  Folder,
  Image,
  Terminal,
  X,
} from "lucide-preact";
import { usePushDirection } from "../../shared/motion.ts";
import type { FileTarget, Session } from "../../shared/types.ts";
import { failure } from "../../shared/types.ts";
import { api, routes } from "../../state/api.ts";
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
import { ImageViewer } from "../../shared/attachments.tsx";
import { ResizeHandle } from "../../shared/side-pane.tsx";
import { MarkdownFile } from "../../shared/markdown-view.tsx";
import { diffLineClass, folderOf, formatStat } from "../../shared/display.ts";
import DiffView from "../../shared/diff-view.tsx";
import { shareOrDownload } from "../../shared/share.ts";
import { HTML, IMAGE, MARKDOWN } from "../../shared/file-types.ts";
import { useHostContext } from "../../state/host-context.ts";
import { useSnapshots } from "../../state/snapshot-store.ts";
import type { Node } from "../../shared/types.ts";
import Canvas, { rows } from "./canvas.tsx";
import System from "./system.tsx";
import OpenPage from "./page.tsx";

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
// Typing pauses this long before the folder is searched.
const SEARCH_DELAY_MS = 150;
const nameOf = (path: string) => path.slice(path.lastIndexOf("/") + 1);
const dirOf = (path: string) =>
  path.slice(0, Math.max(0, path.lastIndexOf("/")));
const join = (dir: string, name: string) => (dir ? `${dir}/${name}` : name);
// "12" or "12-20" as the first and last line.
function lineRange(lines?: string): [number, number] | undefined {
  if (!lines) return undefined;
  const [first, last = first] = lines.split("-").map(Number);
  return [Math.min(first, last), Math.max(first, last)];
}
// Where a path a message wrote leads in `folder`: a file at its lines (or
// its change), a folder, or outside. The pane starts there when it opens
// for a citation, so it never shows the list on the way.
function place({ path: written, lines, changes }: FileTarget, folder: string) {
  const path = written.startsWith("/")
    ? written.startsWith(`${folder}/`)
      ? written.slice(folder.length + 1)
      : undefined
    : written.startsWith("~")
      ? undefined
      : written.replace(/^(\.\/)+/, "");
  if (path === undefined)
    return { dir: "", file: "", showing: "file" as const, outside: written };
  if (path.endsWith("/"))
    return { dir: path.slice(0, -1), file: "", showing: "list" as const };
  const range = lineRange(lines);
  return {
    dir: dirOf(path),
    file: path,
    showing: "file" as const,
    changing: !!changes,
    source: !!range && MARKDOWN.test(path),
    // Kept without lines too, so a file not found is still looked for.
    cited: { path, lines: range },
  };
}

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
// for text and code, rendered markdown and images. A turn that ends may
// have changed the folder, so what is open is read again then.
export default function Files({
  session,
  close,
  openTerminal,
  reveal,
  addToChat,
}: {
  session: Session;
  // Docked beside the chat; in a sheet, the sheet's own close.
  close?: () => void;
  // Opens a terminal in the folder shown, by its path in this one; none
  // while the host offers no terminal.
  openTerminal?: (path: string) => void;
  // A file a message cited, to show at its lines; `at` makes a repeat new.
  reveal?: (FileTarget & { at: number }) | null;
  // Puts text in the conversation's draft; none where there is no draft.
  addToChat?: (text: string, keep?: boolean) => void;
}) {
  // In a sheet, its header is this one: Close plays the sheet's exit, and
  // the title is where the sheet puts focus.
  const dialog = useDialog();
  const closeFiles = close ?? dialog?.close;
  const id = session.id;
  const turn = session.turn_active;
  const view = useContext(ImageViewer);
  const [start] = useState(() => reveal && place(reveal, folderOf(session)));
  const [dir, setDir] = useState(start?.dir ?? "");
  const [file, setFile] = useState(start?.file ?? "");
  const [query, setQuery] = useState("");
  const [index, setIndex] = useState(0);
  const [source, setSource] = useState(!!start?.source);
  // The folder's files, or what differs in it from the last commit.
  const [changing, setChanging] = useState(!!start?.changing);
  // In a narrow pane only one side shows: the list, or the open file.
  const [showing, setShowing] = useState<"list" | "file">(
    start?.showing ?? "list",
  );
  // A cited file and its lines: marked while it is open. Not there as
  // written, the search looks for it and the lines wait for the pick.
  const [cited, setCited] = useState<
    { path: string; lines?: [number, number] } | undefined
  >(start?.cited);
  // A cited path outside the folder, which the pane cannot show.
  const [outside, setOutside] = useState(start?.outside ?? "");
  // The folder's pages side by side, rather than its list.
  const [canvas, setCanvas] = useState(false);
  // A design system's folder as one page: README, tokens, previews.
  const [system, setSystem] = useState(false);
  // A folder a message cited opens as a canvas when it holds pages.
  const [canvasFor, setCanvasFor] = useState(
    start && !start.file && !start.outside ? start.dir : undefined,
  );
  const moved = usePushDirection(showing === "file");
  const { store } = useHostContext();
  const written = useSnapshots(store, (all) =>
    lastWrite(all[id]?.state?.view?.nodes),
  );
  // Each page's version: a write to it reloads its frame.
  const [versions, setVersions] = useState<Record<string, number>>({});
  const preview = useResource(
    () => api<{ base: string }>(routes.preview(id), {}),
    [id],
  );
  const pageUrl = (path: string) =>
    preview.value &&
    `${preview.value.base}${path.split("/").map(encodeURIComponent).join("/")}?v=${versions[path] ?? 0}.${versions[""] ?? 0}`;
  const url = (path: string) => routes.files(id, "raw", path);
  const listing = useResource(
    () => api<Listing>(routes.files(id, "list", dir)),
    [id, dir, turn, written],
  );
  const search = query.trim();
  const found = useResource<Found | undefined>(
    (signal) =>
      search
        ? // A cited path is looked for at once; typing pauses first.
          new Promise((done) =>
            setTimeout(done, search === cited?.path ? 0 : SEARCH_DELAY_MS),
          ).then(() =>
            signal.aborted
              ? undefined
              : api<Found>(routes.files(id, "find", search)).then((value) => ({
                  ...value,
                  query: search,
                })),
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
  const image = IMAGE.test(file) && !changing;
  const page = HTML.test(file) && !changing && !source;
  // The canvas, or one page, across the whole pane.
  const stage = ((canvas || system) && !file) || page;
  const content = useResource(
    () =>
      file && !image && !page && !changing
        ? api<FileText>(routes.files(id, "read", file)).then((value) => ({
            ...value,
            path: file,
          }))
        : undefined,
    [id, file, turn, changing, page],
  );
  const diff = useResource(
    () =>
      file && changing
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
  const marked = cited?.path === file ? cited.lines : undefined;
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
  // The first cited line sits in the middle of the view, once per citation.
  const codeView = useRef<HTMLPreElement>(null);
  const scrolled = useRef<object | undefined>(undefined);
  useLayoutEffect(() => {
    const first = codeView.current?.querySelector("[data-target]");
    if (!cited || !first || scrolled.current === cited) return;
    scrolled.current = cited;
    first.scrollIntoView({ block: "center" });
  }, [code, cited]);

  const results = found.value
    ? fuzzy(found.value.paths, search, (path) => path).slice(0, SHOWN)
    : [];
  const open = (path: string, lines?: [number, number]) => {
    if (path.endsWith("/")) return browse(path.slice(0, -1));
    setFile(path);
    setDir(dirOf(path));
    // Rendered Markdown has no lines to mark: cited lines open its source.
    setSource(!!lines && (MARKDOWN.test(path) || HTML.test(path)));
    setQuery("");
    setShowing("file");
    setCited(lines && { path, lines });
    setOutside("");
  };
  const pick = (next: boolean) => {
    setChanging(next);
    setFile("");
    setShowing("list");
    setCited(undefined);
  };
  const browse = (path: string) => {
    setCanvas(false);
    setSystem(false);
    setDir(path);
    setQuery("");
    setShowing("list");
    setCited(undefined);
    setOutside("");
  };
  const show = (target: FileTarget) => {
    const next = place(target, folderOf(session));
    setDir(next.dir);
    setFile(next.file);
    setShowing(next.showing);
    setChanging(!!next.changing);
    setSource(!!next.source);
    setCited(next.cited);
    setOutside(next.outside ?? "");
    setQuery("");
    setCanvas(false);
    setSystem(false);
    setCanvasFor(next.file || next.outside ? undefined : next.dir);
  };
  const pages = (listing.value?.entries ?? [])
    .filter((entry) => !entry.dir && HTML.test(entry.name))
    .map((entry) => entry.name);
  // biome-ignore lint/correctness/useExhaustiveDependencies: once the listing answers
  useEffect(() => {
    if (canvasFor === undefined || canvasFor !== dir || !listing.value) return;
    setCanvasFor(undefined);
    if (pages.length) setCanvas(true);
  }, [listing.value, canvasFor]);
  // A write reloads the page it changed.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on each write
  useEffect(() => {
    const target = written.slice(written.indexOf("\n") + 1);
    if (!target) return;
    const path = place({ path: target }, folderOf(session)).file;
    // A page, or what pages share (a stylesheet, an image): all reload.
    const key = path && HTML.test(path) ? path : "";
    if (path) setVersions((all) => ({ ...all, [key]: (all[key] ?? 0) + 1 }));
  }, [written]);
  const handled = useRef(reveal?.at ?? 0);
  // biome-ignore lint/correctness/useExhaustiveDependencies: each request once
  useEffect(() => {
    if (reveal && handled.current !== reveal.at) {
      handled.current = reveal.at;
      show(reveal);
    }
  }, [reveal]);
  // A cited change that is no longer one opens as the file.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a failed diff
  useEffect(() => {
    if (
      cited?.path === file &&
      diff.error &&
      failure(diff.error).status === 404
    )
      setChanging(false);
  }, [diff.error]);
  // A cited file not there as written: the search looks for its path while
  // the viewer says so; the list shows only if it finds none or several.
  const missing =
    cited?.path === file &&
    !!content.error &&
    failure(content.error).status === 404;
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a failed read
  useEffect(() => {
    if (missing) {
      setFile("");
      setQuery(cited!.path);
    }
  }, [missing]);
  const looking = !!cited && cited.path !== file && search === cited.path;
  const answered = looking && found.value?.query === search;
  // The files whose path ends with the cited one; a single one opens.
  const named = answered
    ? found.value!.paths.filter(
        (path) => path === cited.path || path.endsWith(`/${cited.path}`),
      )
    : [];
  // Lines a pick from the search opens at, while a citation waits for it.
  const waiting = cited && cited.path !== file ? cited.lines : undefined;
  // biome-ignore lint/correctness/useExhaustiveDependencies: once the search answers
  useEffect(() => {
    if (!answered) return;
    if (named.length !== 1) return setShowing("list");
    open(named[0], waiting);
    setCited({ path: named[0], lines: waiting });
  }, [answered]);
  // Still looking: the title keeps the cited name, the viewer says so.
  const seeking = (looking && (!answered || named.length === 1)) || missing;
  const titled = file || (seeking ? cited!.path : canvas || system ? dir : "");
  const crumbs = dir ? dir.split("/") : [];
  const folderName = nameOf(folderOf(session)) || "Files";
  const fileError = content.error ? failure(content.error) : undefined;

  return (
    <section class="files" aria-label="Files" data-showing={showing}>
      <header class="files-head">
        {page && (
          <IconButton
            class="files-back files-back-canvas"
            label="Back to the canvas"
            onClick={() => {
              setFile("");
              setCanvas(true);
            }}
          >
            <ChevronLeft />
          </IconButton>
        )}
        {(file || outside) && showing === "file" && !page && (
          <IconButton
            class="files-back"
            label="Back to the list"
            onClick={() => setShowing("list")}
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
              <span class="files-name">{nameOf(titled)}</span>
            </>
          ) : (
            folderName
          )}
        </h2>
        {diffText && formatStat(counts) && (
          <span class="files-dir small">{formatStat(counts)}</span>
        )}
        {!file && !changing && !search && pages.length > 0 && (
          <Button
            variant="secondary"
            aria-pressed={canvas}
            onClick={() => {
              setCanvas(!canvas);
              setSystem(false);
            }}
          >
            Canvas
          </Button>
        )}
        {!file &&
          !changing &&
          !search &&
          listing.value?.entries.some(
            (entry) => entry.name === "tokens.css",
          ) && (
            <Button
              variant="secondary"
              aria-pressed={system}
              onClick={() => {
                setSystem(!system);
                setCanvas(false);
              }}
            >
              System
            </Button>
          )}
        {file && !changing && (MARKDOWN.test(file) || HTML.test(file)) && (
          <Button
            variant="secondary"
            aria-pressed={source}
            onClick={() => setSource(!source)}
          >
            Source
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
            title="Download"
          >
            <Download />
          </a>
        )}
        {openTerminal && !file && !changing && (
          <IconButton
            label="Open terminal here"
            onClick={() => openTerminal(dir)}
          >
            <Terminal />
          </IconButton>
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
                names.map((name) => join(dirOf(file), name)),
              )}
              go={open}
              addToChat={addToChat}
              base={preview.value.base}
              read={(path) =>
                api<FileText>(routes.files(id, "read", path)).then(
                  (value) => value.text,
                )
              }
            />
          ) : system ? (
            <System
              id={id}
              dir={dir}
              turn={`${turn}${written}`}
              page={(path) => pageUrl(path)!}
              url={url}
            />
          ) : (
            <Canvas
              dir={dir}
              names={pages}
              page={(path) => pageUrl(path)!}
              open={open}
              addToChat={addToChat}
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
                aria-pressed={!changing}
                onClick={() => pick(false)}
              >
                Files
              </Button>
              <Button
                variant="secondary"
                aria-pressed={changing}
                onClick={() => pick(true)}
              >
                {`Changes ${changes.length}`}
              </Button>
            </div>
          )}
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
              setCited(undefined);
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
                open(results[index], waiting);
              else return;
              event.preventDefault();
              event.stopPropagation();
            }}
          />
          {search ? (
            <>
              {named.length > 1 && (
                <p class="files-note small">
                  {`${named.length} files are named ${nameOf(cited!.path)}.`}
                  {waiting && ` Pick one to open it at line ${waiting[0]}.`}
                </p>
              )}
              <SuggestionList
                id="files-results"
                class="files-results"
                label="Matching files"
                prefix="files-result"
                items={results}
                index={index}
                pick={(path) => open(path, waiting)}
                keyOf={(path) => path}
              >
                {(path) => (
                  <>
                    <span class="files-name">{nameOf(path)}</span>
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
          ) : changing ? (
            <>
              {changes.length ? (
                <ul class="files-entries">
                  {changes.map((change) => (
                    <li key={change.path}>
                      <Button
                        variant="quiet"
                        class="entry"
                        aria-current={change.path === file ? "true" : undefined}
                        onClick={() => open(change.path)}
                      >
                        <span class="files-state" data-state={change.state}>
                          {change.state}
                        </span>
                        <span class="files-name">{change.path}</span>
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
              <div class="files-crumbs">
                {["", ...crumbs].map((name, at) => {
                  const path = crumbs.slice(0, at).join("/");
                  return (
                    <Button
                      key={path}
                      variant="quiet"
                      aria-current={path === dir ? "location" : undefined}
                      onClick={() => browse(path)}
                    >
                      {name || folderName}
                    </Button>
                  );
                })}
              </div>
              {listing.error ? (
                <LoadError error={listing.error} retry={listing.retry} />
              ) : listing.value && !listing.value.entries.length ? (
                <EmptyState>This folder is empty.</EmptyState>
              ) : (
                <Placeholder label="Loading files…" when={!listing.value}>
                  <ul class="files-entries">
                    {(listing.value ?? SAMPLE_LISTING).entries.map((entry) => {
                      const path = join(dir, entry.name);
                      return (
                        <li key={entry.name}>
                          <Button
                            variant="quiet"
                            class="entry"
                            aria-current={path === file ? "true" : undefined}
                            onClick={() =>
                              entry.dir ? browse(path) : open(path)
                            }
                          >
                            {entry.dir ? (
                              <Folder />
                            ) : IMAGE.test(entry.name) ? (
                              <Image />
                            ) : (
                              <File />
                            )}
                            <span class="files-name">{entry.name}</span>
                          </Button>
                        </li>
                      );
                    })}
                  </ul>
                </Placeholder>
              )}
              {listing.value?.more && (
                <p class="files-note small">
                  Some entries may not be listed. Search to find the rest.
                </p>
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
              open(link.dataset.file!);
            }
            const reference = target.closest<HTMLElement>(".file-ref");
            if (reference)
              show({
                path: reference.dataset.file!,
                lines: reference.dataset.lines,
              });
            if (
              target instanceof HTMLImageElement &&
              target.closest(".markdown")
            )
              view({ src: target.src, name: target.alt || nameOf(file) });
          }}
        >
          {outside ? (
            <EmptyState>
              {`${outside} is outside this conversation's folder, so it can't be shown here.`}
            </EmptyState>
          ) : seeking ? (
            <Spinner label={`Looking for ${nameOf(cited!.path)}…`} />
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
            <Button
              variant="quiet"
              class="files-image"
              aria-label={`View ${nameOf(file)}`}
              onClick={() => view({ src: url(file), name: nameOf(file) })}
            >
              <img src={url(file)} alt={nameOf(file)} />
            </Button>
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
