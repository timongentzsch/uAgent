import { useCallback, useLayoutEffect, useRef, useState } from "preact/hooks";
import { ChevronRight, Folder, Pencil } from "lucide-preact";
import { useApi } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import {
  Button,
  IconButton,
  Input,
  LoadError,
  Switch,
} from "../../shared/ui.tsx";
import { DataText, Placeholder } from "../../shared/placeholder.tsx";
import { type Listing, SAMPLE_LISTING } from "./tree.tsx";
import "./files.css";
import "./folder-picker.css";

interface Folders extends Listing {
  path: string;
  parent: string;
}

const HOME = "~";
const SAMPLE: Folders = { ...SAMPLE_LISTING, path: "/home", parent: "/" };

// The folders from the top down to `path`, each with where it leads; one
// under the home folder starts at "~".
function crumbsOf(path: string, home: string) {
  const under = !!home && (path === home || path.startsWith(`${home}/`));
  const base = under ? home : "/";
  const crumbs = [{ name: under ? HOME : "/", path: base }];
  let at = under ? home : "";
  for (const name of path.slice(base.length).split("/").filter(Boolean)) {
    at = `${at}/${name}`;
    crumbs.push({ name, path: at });
  }
  return crumbs;
}
const within = (folder: string, name: string) =>
  `${folder.replace(/\/$/, "")}/${name}`;

// A host's folders as a browser: where you are as crumbs, what is in it as
// a list. A click chooses a folder; its arrow or a double click goes in.
export default function FolderPicker({
  value,
  change,
  online,
  projects,
}: {
  value: string;
  change: (path: string) => void;
  online: boolean;
  projects: string[];
}) {
  const { api, routes } = useApi();
  const [root, setRoot] = useState(HOME);
  const [home, setHome] = useState("");
  const [typing, setTyping] = useState(false);
  const [hidden, setHidden] = useState(false);
  const editing = useRef(false);
  const list = useCallback(
    (path: string, signal: AbortSignal) =>
      api<Folders>(routes.folders(path), undefined, { signal }),
    [api, routes],
  );
  const resource = useResource(
    (signal) => (online ? list(root, signal) : undefined),
    [list, root, online],
  );
  const listing = resource.value;
  // A folder that has loaded is the one chosen, under its full name, unless
  // a path is being typed: that is not replaced under the caret.
  useLayoutEffect(() => {
    if (!listing) return;
    if (root === HOME) setHome(listing.path);
    if (!editing.current && (!value || value === root)) change(listing.path);
  }, [listing, root, value, change]);
  const navigate = (path: string) => {
    editing.current = false;
    setTyping(false);
    setRoot(path);
    change(path);
    resource.setValue(undefined);
    resource.retry();
  };
  const row = (name: string, path: string) => (
    <li key={path} class="tree-folder" data-path={path}>
      <Button
        variant="quiet"
        class="entry tree-row"
        aria-current={path === value ? "true" : undefined}
        onClick={() => change(path)}
        onDblClick={() => navigate(path)}
      >
        <Folder />
        <span class="files-name truncate">
          <DataText>{name}</DataText>
        </span>
      </Button>
      <IconButton label={`Open ${name}`} onClick={() => navigate(path)}>
        <ChevronRight />
      </IconButton>
    </li>
  );
  const shown = listing ?? SAMPLE;
  const folders = shown.entries.filter(
    (entry) => entry.dir && (hidden || !entry.name.startsWith(".")),
  );
  const tucked = shown.entries.length - folders.length;
  return (
    <div class="folder-picker">
      {typing ? (
        <label>
          Folder on the host
          <Input
            autoFocus
            value={value}
            onInput={(event) => change(event.currentTarget.value)}
            onBlur={() => {
              const path = value.trim();
              if (path && path !== listing?.path) navigate(path);
              else setTyping(false);
            }}
            placeholder="A path on the host"
            disabled={!online}
          />
        </label>
      ) : (
        <nav class="folder-crumbs" aria-label="Folder on the host">
          {(listing ? crumbsOf(listing.path, home) : []).map(
            (crumb, at, all) => (
              <Button
                key={crumb.path}
                variant="quiet"
                size="compact"
                aria-current={at === all.length - 1 ? "location" : undefined}
                disabled={!online}
                onClick={() => navigate(crumb.path)}
              >
                {crumb.name}
              </Button>
            ),
          )}
          <IconButton
            label="Type a path"
            disabled={!online}
            onClick={() => {
              editing.current = true;
              setTyping(true);
            }}
          >
            <Pencil />
          </IconButton>
        </nav>
      )}
      {!online ? (
        <p class="small muted">Connect this host to browse its folders.</p>
      ) : (
        <div class="folder-list">
          {root === HOME && projects.length > 0 && (
            <>
              <h3 class="caption muted">Recent on this host</h3>
              <ul class="files-tree dense" aria-label="Recent on this host">
                {projects.map((path) =>
                  row(
                    home && path.startsWith(`${home}/`)
                      ? HOME + path.slice(home.length)
                      : path,
                    path,
                  ),
                )}
              </ul>
              <h3 class="caption muted">Home</h3>
            </>
          )}
          {resource.error ? (
            <LoadError error={resource.error} retry={resource.retry} />
          ) : (
            <Placeholder label="Loading folders…" when={!listing}>
              <ul class="files-tree dense">
                {folders.map((entry) =>
                  row(entry.name, within(shown.path, entry.name)),
                )}
              </ul>
              {!folders.length && (
                <p class="small muted">
                  {tucked
                    ? "Only hidden folders here."
                    : "No folders here. You can start in this one."}
                </p>
              )}
            </Placeholder>
          )}
        </div>
      )}
      <div class="folder-foot small muted">
        <span class="folder-picker-path">{value}</span>
        <label>
          Hidden folders
          <Switch
            label="Show hidden folders"
            checked={hidden}
            onChange={setHidden}
          />
        </label>
      </div>
    </div>
  );
}
