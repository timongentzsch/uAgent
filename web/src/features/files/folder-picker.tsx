import { useCallback, useLayoutEffect, useRef, useState } from "preact/hooks";
import { ArrowUp, Home } from "lucide-preact";
import { useApi } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import {
  Button,
  IconButton,
  Input,
  LoadError,
  Spinner,
} from "../../shared/ui.tsx";
import FileTree, { type Listing } from "./tree.tsx";
import "./files.css";
import "./folder-picker.css";

interface Folders extends Listing {
  path: string;
  parent: string;
}

// Uses the same folder tree as Files, with directories supplied by the chosen host.
export default function FolderPicker({
  value,
  change,
  online,
}: {
  value: string;
  change: (path: string) => void;
  online: boolean;
}) {
  const { api, routes, host } = useApi();
  const [root, setRoot] = useState("~");
  const [expanded, setExpanded] = useState(new Set<string>());
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
  // Loading home must not replace the value or caret once path entry starts.
  useLayoutEffect(() => {
    if (listing && !editing.current && (!value || value === root))
      change(listing.path);
  }, [listing, root, value, change]);
  const navigate = (path: string) => {
    editing.current = false;
    setRoot(path);
    setExpanded(new Set());
    change(path);
    resource.setValue(undefined);
    resource.retry();
  };
  return (
    <div class="folder-picker">
      <label>
        Folder on the host
        <Input
          value={value}
          onFocus={() => {
            editing.current = true;
          }}
          onInput={(event) => {
            editing.current = true;
            change(event.currentTarget.value);
          }}
          placeholder="Choose a folder below, or enter a path"
          disabled={!online}
        />
      </label>
      <div class="folder-picker-actions">
        <IconButton
          label="Home folder"
          disabled={!online}
          onClick={() => navigate("~")}
        >
          <Home />
        </IconButton>
        <IconButton
          label="Parent folder"
          disabled={!online || !listing || listing.parent === listing.path}
          onClick={() => listing && navigate(listing.parent)}
        >
          <ArrowUp />
        </IconButton>
        <Button
          type="button"
          disabled={!online || !value.trim()}
          onClick={() => navigate(value.trim())}
        >
          Browse path
        </Button>
      </div>
      {!online ? (
        <p class="small muted">Connect this host to browse its folders.</p>
      ) : resource.error ? (
        <LoadError error={resource.error} retry={resource.retry} />
      ) : listing ? (
        <>
          <p class="small muted folder-picker-path">{listing.path}</p>
          <FileTree
            session=""
            version={host}
            root={listing}
            rootPath={listing.path}
            expanded={expanded}
            dir={value}
            file=""
            list={list}
            toggle={(path) => {
              change(path);
              setExpanded((prior) => {
                const next = new Set(prior);
                if (next.has(path)) next.delete(path);
                else next.add(path);
                return next;
              });
            }}
            open={() => {}}
            canvas={() => {}}
          />
          {!listing.entries.length && (
            <p class="small muted">No subfolders. You can use this folder.</p>
          )}
        </>
      ) : (
        <Spinner label="Loading folders…" />
      )}
    </div>
  );
}
