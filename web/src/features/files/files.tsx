import "./files.css";
import { useContext, useState } from "preact/hooks";
import { ChevronLeft, Download, File, Folder, Image, X } from "lucide-preact";
import type { Session } from "../../shared/types.ts";
import { failure } from "../../shared/types.ts";
import { api, routes } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import {
  Button,
  IconButton,
  LoadError,
  Placeholder,
  Spinner,
} from "../../shared/ui.tsx";
import { Input } from "../../shared/form-controls.tsx";
import { SuggestionList } from "../../shared/suggestion-list.tsx";
import { fuzzy } from "../../shared/fuzzy.ts";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { ImageViewer } from "../../shared/attachments.tsx";
import { ResizeHandle } from "../../shared/side-pane.tsx";
import { MarkdownFile } from "../../shared/markdown-view.tsx";
import { folderOf } from "../../shared/display.ts";

interface Listing {
  entries: { name: string; dir: boolean; size: number }[];
  more: boolean;
}
interface Found {
  paths: string[];
  more: boolean;
}
interface FileText {
  text: string;
  bytes: number;
  more: boolean;
}

const IMAGE = /\.(png|jpe?g|gif|webp|svg)$/i;
const MARKDOWN = /\.(md|markdown)$/i;
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

// The conversation's folder, read only: a list with search beside a viewer
// for text and code, rendered markdown and images. A turn that ends may
// have changed the folder, so what is open is read again then.
export default function Files({
  session,
  close,
}: {
  session: Session;
  // Docked beside the chat; a sheet has its own Close.
  close?: () => void;
}) {
  const id = session.id;
  const turn = session.turn_active;
  const view = useContext(ImageViewer);
  const [dir, setDir] = useState("");
  const [file, setFile] = useState("");
  const [query, setQuery] = useState("");
  const [index, setIndex] = useState(0);
  const [source, setSource] = useState(false);
  // In a narrow pane only one side shows: the list, or the open file.
  const [showing, setShowing] = useState<"list" | "file">("list");
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
  const image = IMAGE.test(file);
  const content = useResource(
    () =>
      file && !image
        ? api<FileText>(routes.files(id, "read", file)).then((value) => ({
            ...value,
            path: file,
          }))
        : undefined,
    [id, file, turn],
  );
  const text = content.value?.path === file ? content.value : undefined;
  const markdown = MARKDOWN.test(file) && !source;
  const lines = useResource(
    () =>
      text && !markdown
        ? import("../../shared/highlight.ts").then(({ highlightLines }) => ({
            path: file,
            html: highlightLines(text.text, file)
              .map(
                (line, at) =>
                  `<span class="line-number">${at + 1}</span><span class="line">${line}\n</span>`,
              )
              .join(""),
          }))
        : undefined,
    [text, markdown],
  );

  const results = found.value
    ? fuzzy(found.value.paths, search, (path) => path).slice(0, SHOWN)
    : [];
  const open = (path: string) => {
    if (path.endsWith("/")) return browse(path.slice(0, -1));
    setFile(path);
    setDir(dirOf(path));
    setSource(false);
    setQuery("");
    setShowing("file");
  };
  const browse = (path: string) => {
    setDir(path);
    setQuery("");
    setShowing("list");
  };
  const crumbs = dir ? dir.split("/") : [];
  const folderName = nameOf(folderOf(session)) || "Files";
  const fileError = content.error ? failure(content.error) : undefined;

  return (
    <section class="files" aria-label="Files" data-showing={showing}>
      <header class="files-head">
        {file && showing === "file" && (
          <IconButton
            class="files-back"
            label="Back to the list"
            onClick={() => setShowing("list")}
          >
            <ChevronLeft />
          </IconButton>
        )}
        <h2 class="files-title" title={file || undefined}>
          {file ? (
            <>
              <span class="files-dir">{dirOf(file) && `${dirOf(file)}/`}</span>
              <span class="files-name">{nameOf(file)}</span>
            </>
          ) : (
            folderName
          )}
        </h2>
        {file && MARKDOWN.test(file) && (
          <Button
            variant="secondary"
            aria-pressed={source}
            onClick={() => setSource(!source)}
          >
            Source
          </Button>
        )}
        {file && (
          <a
            class="icon-button files-download"
            href={`${url(file)}&download=1`}
            download={nameOf(file)}
            aria-label={`Download ${nameOf(file)}`}
            title="Download"
          >
            <Download />
          </a>
        )}
        {close && (
          <IconButton label="Close files" onClick={close}>
            <X />
          </IconButton>
        )}
      </header>
      <div class="files-columns">
        <nav class="files-list" aria-label="Folder">
          <ResizeHandle
            pane="files-list"
            label="Resize the file list"
            edge="end"
          />
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
                open(results[index]);
              else return;
              event.preventDefault();
              event.stopPropagation();
            }}
          />
          {search ? (
            <>
              <SuggestionList
                id="files-results"
                class="files-results"
                label="Matching files"
                prefix="files-result"
                items={results}
                index={index}
                pick={open}
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
                <p class="files-note">No file name matches.</p>
              ) : (
                (found.value?.more ||
                  (found.value && found.value.paths.length > SHOWN)) && (
                  <p class="files-note">
                    Some entries may not be listed. Keep typing to narrow.
                  </p>
                )
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
              ) : (
                <Placeholder label="Loading files…" when={!listing.value}>
                  <ul class="files-entries">
                    {(listing.value ?? SAMPLE_LISTING).entries.map((entry) => {
                      const path = join(dir, entry.name);
                      return (
                        <li key={entry.name}>
                          <Button
                            variant="quiet"
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
                    {listing.value && !listing.value.entries.length && (
                      <li class="files-note">This folder is empty.</li>
                    )}
                  </ul>
                </Placeholder>
              )}
              {listing.value?.more && (
                <p class="files-note">
                  Some entries may not be listed. Search to find the rest.
                </p>
              )}
            </>
          )}
        </nav>
        <div
          class="files-view"
          // Delegated: the rendered markdown's file links and images.
          onClick={(event) => {
            const target = event.target as HTMLElement;
            const link = target.closest<HTMLAnchorElement>("a[data-file]");
            if (link && !(event.metaKey || event.ctrlKey)) {
              event.preventDefault();
              open(link.dataset.file!);
            }
            if (
              target instanceof HTMLImageElement &&
              target.closest(".markdown")
            )
              view({ src: target.src, name: target.alt || nameOf(file) });
          }}
        >
          {!file ? (
            <p class="files-note">Choose a file to read it here.</p>
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
                <a href={`${url(file)}&download=1`} download={nameOf(file)}>
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
              ) : lines.value?.path === file ? (
                <pre
                  class="code-view"
                  // The highlighter's own spans, escaped by it.
                  dangerouslySetInnerHTML={{ __html: lines.value.html }}
                />
              ) : (
                <Spinner label="Highlighting…" />
              )}
              {text.more && (
                <p class="files-note files-cut">
                  Showing the first part of this file.{" "}
                  <a href={`${url(file)}&download=1`} download={nameOf(file)}>
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
