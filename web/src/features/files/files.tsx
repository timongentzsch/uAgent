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
import { IMAGE, MARKDOWN } from "../../shared/file-types.ts";

// The Files pane: a folder's tree, search and previews of text, images and
// Markdown, read from the host. Docked beside the chat or a sheet on a phone.

interface Listing {
  entries: { name: string; dir: boolean; size: number }[];
  more: boolean;
}
interface Found {
  paths: string[];
  more: boolean;
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

// The conversation's folder, read only: a list with search beside a viewer
// for text and code, rendered markdown and images. A turn that ends may
// have changed the folder, so what is open is read again then.
export default function Files({
  session,
  close,
  openTerminal,
  reveal,
}: {
  session: Session;
  // Docked beside the chat; in a sheet, the sheet's own close.
  close?: () => void;
  // Opens a terminal in the folder shown, by its path in this one; none
  // while the host offers no terminal.
  openTerminal?: (path: string) => void;
  // A file a message cited, to show at its lines; `at` makes a repeat new.
  reveal?: (FileTarget & { at: number }) | null;
}) {
  // In a sheet, its header is this one: Close plays the sheet's exit, and
  // the title is where the sheet puts focus.
  const dialog = useDialog();
  const closeFiles = close ?? dialog?.close;
  const id = session.id;
  const turn = session.turn_active;
  const view = useContext(ImageViewer);
  const [dir, setDir] = useState("");
  const [file, setFile] = useState("");
  const [query, setQuery] = useState("");
  const [index, setIndex] = useState(0);
  const [source, setSource] = useState(false);
  // The folder's files, or what differs in it from the last commit.
  const [changing, setChanging] = useState(false);
  // In a narrow pane only one side shows: the list, or the open file.
  const [showing, setShowing] = useState<"list" | "file">("list");
  // A cited file and its lines: marked while it is open. Not there as
  // written, the search looks for it and the lines wait for the pick.
  const [cited, setCited] = useState<{
    path: string;
    lines?: [number, number];
  }>();
  // A cited path outside the folder, which the pane cannot show.
  const [outside, setOutside] = useState("");
  const moved = usePushDirection(showing === "file");
  const url = (path: string) => routes.files(id, "raw", path);
  const listing = useResource(
    () => api<Listing>(routes.files(id, "list", dir)),
    [id, dir, turn],
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
  const image = IMAGE.test(file) && !changing;
  const content = useResource(
    () =>
      file && !image && !changing
        ? api<FileText>(routes.files(id, "read", file)).then((value) => ({
            ...value,
            path: file,
          }))
        : undefined,
    [id, file, turn, changing],
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
    setSource(!!lines && MARKDOWN.test(path));
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
    setDir(path);
    setQuery("");
    setShowing("list");
    setCited(undefined);
    setOutside("");
  };
  // A path as a message wrote it: in this folder, absolute inside it, or
  // outside it; a folder, a change, or a file at its lines.
  const show = ({ path: written, lines, changes: diffOf }: FileTarget) => {
    const folder = folderOf(session);
    const path = written.startsWith("/")
      ? written.startsWith(`${folder}/`)
        ? written.slice(folder.length + 1)
        : undefined
      : written.startsWith("~")
        ? undefined
        : written.replace(/^(\.\/)+/, "");
    if (path === undefined) {
      setFile("");
      setCited(undefined);
      setOutside(written);
      setShowing("file");
      return;
    }
    setChanging(!!diffOf);
    if (path.endsWith("/")) return browse(path.slice(0, -1));
    open(path, lineRange(lines));
    // Kept without lines too, so a file not found is still looked for.
    setCited({ path, lines: lineRange(lines) });
  };
  const handled = useRef(0);
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
  // A cited file not there as written: the search looks for its path.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on a failed read
  useEffect(() => {
    if (
      cited?.path === file &&
      content.error &&
      failure(content.error).status === 404
    ) {
      setFile("");
      setQuery(cited.path);
      setShowing("list");
    }
  }, [content.error]);
  // The files whose path ends with the cited one; a single one opens.
  const named =
    cited && cited.path !== file && search === cited.path && found.value
      ? found.value.paths.filter(
          (path) => path === cited.path || path.endsWith(`/${cited.path}`),
        )
      : [];
  // Lines a pick from the search opens at, while a citation waits for it.
  const waiting = cited && cited.path !== file ? cited.lines : undefined;
  // biome-ignore lint/correctness/useExhaustiveDependencies: once the search answers
  useEffect(() => {
    if (named.length !== 1) return;
    open(named[0], waiting);
    setCited({ path: named[0], lines: waiting });
  }, [found.value]);
  const crumbs = dir ? dir.split("/") : [];
  const folderName = nameOf(folderOf(session)) || "Files";
  const fileError = content.error ? failure(content.error) : undefined;

  return (
    <section class="files" aria-label="Files" data-showing={showing}>
      <header class="files-head">
        {(file || outside) && showing === "file" && (
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
          title={file || undefined}
          ref={dialog?.heading}
          tabIndex={dialog ? -1 : undefined}
        >
          {file ? (
            <>
              <span class="files-dir">{dirOf(file) && `${dirOf(file)}/`}</span>
              <span class="files-name">{nameOf(file)}</span>
            </>
          ) : (
            folderName
          )}
        </h2>
        {diffText && formatStat(counts) && (
          <span class="files-dir small">{formatStat(counts)}</span>
        )}
        {file && !changing && MARKDOWN.test(file) && (
          <Button
            variant="secondary"
            aria-pressed={source}
            onClick={() => setSource(!source)}
          >
            Source
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
      <div
        class="files-columns"
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
