import { useAction } from "../../shared/use-action.ts";
import { useLayoutEffect, useMemo, useState } from "preact/hooks";
import { ChevronLeft, Pencil } from "lucide-preact";
import { tabDrafts } from "../../shared/storage.ts";
import { storageKeys, hostStorageKey } from "../../shared/storage-keys.ts";
import { useApi } from "../../state/api.ts";
import type { LibraryItem, LibraryPlace } from "../../shared/types.ts";
import {
  Actions,
  Button,
  ConfirmModal,
  Field,
  Input,
  LoadError,
  Modal,
  Select,
  Spinner,
  Textarea,
  Time,
} from "../../shared/ui.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { bytes } from "../../shared/quantities.ts";
import { useResource } from "../../shared/use-resource.ts";
import Markdown from "../../shared/markdown-view.tsx";

// One memory or skill: read, edited and saved against the revision it was
// read at, so a change made elsewhere is never overwritten. A new one is
// named and placed first. Unsaved text is kept per tab.

type Kind = "memory" | "skills";
type Draft = {
  item: LibraryItem;
  content: string;
  name: string;
  where: string;
};
const blank = (): LibraryItem => ({
  key: "",
  name: "",
  path: "",
  scope: "project",
  source: "uAgent",
  writable: true,
  revision: "",
  content: "",
});
const placeName = (places: LibraryPlace[], path: string) =>
  path
    ? (places.find((place) => place.path === path)?.name ?? path)
    : "Everywhere";

// Where a memory or skill can live: every place, or one.
function WhereField({
  value,
  places,
  everywhere = true,
  change,
}: {
  value: string;
  places: LibraryPlace[];
  everywhere?: boolean;
  change: (value: string) => void;
}) {
  return (
    <Field label="Where">
      <Select
        value={value}
        onChange={(event) => change(event.currentTarget.value)}
      >
        {everywhere && <option value="">Everywhere</option>}
        {places.map((place) => (
          <option key={place.path} value={place.path} title={place.path}>
            {place.name}
          </option>
        ))}
      </Select>
    </Field>
  );
}

export default function LibraryDocument({
  kind,
  entry,
  cwd,
  places,
  online,
  limit,
  close,
  opened,
  changed: refresh,
}: {
  kind: Kind;
  // The listed item, or a new one's starting text and place.
  entry: LibraryItem | { new: true; content: string; where: string };
  // The folder its controls run in: its own place, or any for one of every
  // place's.
  cwd: string;
  places: LibraryPlace[];
  online: boolean;
  limit?: number;
  close: () => void;
  // A new one, once saved, by its key and place.
  opened?: (key: string, place: string) => void;
  changed: () => void;
}) {
  const { manage, host } = useApi();
  const fresh = "new" in entry;
  // The listing's copy, newer when it changed on disk.
  const listed = fresh ? undefined : entry;
  const drafts = useMemo(
    () =>
      tabDrafts<Draft>(hostStorageKey(storageKeys.library(kind, cwd), host)),
    [kind, cwd, host],
  );
  const key = fresh ? "" : entry.key;
  const loaded = useResource(
    (signal) =>
      fresh
        ? Promise.resolve(blank())
        : manage(kind, { cwd, action: "get", key }, { signal }).then(
            (result) => {
              if (!result.item || result.item.error)
                throw new Error(result.item?.error || "not found");
              return result.item;
            },
          ),
    [kind, cwd, key],
  );
  const [item, setItem] = useState<LibraryItem | null>(null);
  const [content, setContent] = useState("");
  const [name, setName] = useState("");
  const [where, setWhere] = useState(fresh ? entry.where : cwd);
  const [editing, setEditing] = useState(fresh);
  const { run: act, busy, error } = useAction();
  const [dialog, setDialog] = useState<"delete" | "rename" | "move" | null>(
    null,
  );
  const [target, setTarget] = useState("");
  // A loaded item starts from its saved draft, if there is one.
  // biome-ignore lint/correctness/useExhaustiveDependencies: once per load
  useLayoutEffect(() => {
    const value = loaded.value;
    if (!value) return;
    const saved = drafts.read(value.key);
    setItem(saved?.item || value);
    setName(saved?.name ?? value.name);
    setContent(
      saved?.content ?? (fresh ? entry.content : (value.content ?? "")),
    );
    if (saved) setWhere(saved.where);
    setEditing(fresh || !!saved);
  }, [loaded.value]);
  const dirty = !!item && (!item.key || content !== item.content);
  useLayoutEffect(() => {
    if (item && dirty) drafts.keep(item.key, { item, content, name, where });
  }, [item, content, name, where, dirty, drafts]);
  const forget = () => drafts.forget(item?.key);
  const stale = !!(item?.key && listed && item.revision !== listed.revision);
  const run = (task: () => Promise<void>) =>
    act(async () => {
      await task();
      setDialog(null);
      refresh();
    });
  const call = (
    action: string,
    fields: Record<string, unknown> = {},
    at = cwd,
  ) =>
    manage(kind, {
      cwd: at,
      action,
      key: item?.key,
      revision: item?.revision,
      ...fields,
    });
  const show = (saved?: LibraryItem) => {
    if (!saved) return;
    setItem(saved);
    setContent(saved.content || "");
    setEditing(false);
  };
  const save = () =>
    run(async () => {
      // A new one's key says where: a place's own or every place's.
      const scope = where ? "project" : "global";
      const result = await manage(kind, {
        cwd: where || cwd,
        action: "set",
        key: item?.key || `${scope}/${name}`,
        revision: item?.revision,
        content,
      });
      forget();
      if (fresh && result.item) opened?.(result.item.key, where);
      else show(result.item);
    });
  // A memory moves by a copy where it goes and then its removal here: into
  // every place's in its own folder, or into another place's.
  const move = (to: string, keep: boolean) =>
    run(async () => {
      if (!item) return;
      if (!to)
        await call(keep ? "copy" : "rename", { target: `global/${item.name}` });
      else {
        await manage(kind, {
          cwd: to,
          action: "set",
          key: `project/${item.name}`,
          revision: "",
          content: item.content,
        });
        if (!keep) await call("forget");
      }
      if (!keep) close();
    });
  // Paths the importer can take; an item is offered only if it is one.
  const importable = useResource(
    (signal) =>
      item?.key && online
        ? manage(
            "import",
            {
              cwd,
              action: "list",
              only: kind === "skills" ? "skill" : "memory",
            },
            { signal },
          ).then((result) =>
            (result.items || []).some(
              (each) =>
                each.path === item.path && !each.blocked && !each.collides,
            ),
          )
        : undefined,
    [item?.path, cwd, online],
  );
  if (loaded.error)
    return <LoadError error={loaded.error} retry={loaded.retry} />;
  if (!item) return <Spinner label="Loading document…" surface />;
  const off = (listed?.status || item.status) === "disabled";
  const memory = kind === "memory";
  return (
    <fieldset disabled={busy} class="library-document">
      <div class="editor-head">
        <Button variant="quiet" onClick={close}>
          <ChevronLeft />
          Back
        </Button>
        <strong>
          {item.key ? item.name : `New ${memory ? "memory" : "skill"}`}
        </strong>
        {item.key && (
          <Menu label="Library item menu">
            {memory && item.writable && (
              <MenuItem
                onClick={() => {
                  setTarget(
                    item.scope === "global" ? (places[0]?.path ?? "") : "",
                  );
                  setDialog("move");
                }}
              >
                Move to…
              </MenuItem>
            )}
            {memory && item.writable && item.scope === "project" && (
              <MenuItem disabled={!online} onClick={() => move("", true)}>
                Copy to everywhere
              </MenuItem>
            )}
            {kind === "skills" && (
              <MenuItem
                disabled={!online}
                onClick={() =>
                  run(() => call(off ? "enable" : "disable").then(() => {}))
                }
              >
                {off ? "Enable globally" : "Disable globally"}
              </MenuItem>
            )}
            {importable.value && (
              <MenuItem
                disabled={!online}
                onClick={() =>
                  run(async () => {
                    await manage("import", {
                      cwd,
                      action: "run",
                      only: memory ? "memory" : "skill",
                      key: memory ? item.name : item.key,
                    });
                    close();
                  })
                }
              >
                Import into µAgent
              </MenuItem>
            )}
            {item.writable && memory && (
              <MenuItem
                onClick={() => {
                  setTarget(item.name);
                  setDialog("rename");
                }}
              >
                Rename
              </MenuItem>
            )}
            {item.writable && (
              <MenuItem onClick={() => setDialog("delete")}>Delete</MenuItem>
            )}
          </Menu>
        )}
      </div>
      {!item.key ? (
        <div class="field-row">
          <Field label="Name">
            <Input
              value={name}
              onInput={(event) => setName(event.currentTarget.value)}
            />
          </Field>
          <WhereField value={where} places={places} change={setWhere} />
        </div>
      ) : (
        <p class="muted">
          {placeName(places, item.scope === "project" ? cwd : "")} ·{" "}
          {item.source}
          {!item.writable ? " · Read-only" : ""}
        </p>
      )}
      {stale && (
        <p role="status">
          Changed on disk. Your draft is retained.{" "}
          <Button
            onClick={() => {
              forget();
              loaded.retry();
            }}
          >
            Reload
          </Button>
        </p>
      )}
      <div class="segmented document-toolbar">
        {item.writable && !editing && (
          <Button onClick={() => setEditing(true)}>
            <Pencil />
            Edit
          </Button>
        )}
        <small>
          {bytes(new TextEncoder().encode(content).length)}
          {item.writable && limit ? ` / ${bytes(limit)}` : ""}
        </small>
      </div>
      {!editing ? (
        <div class="document-preview">
          <Markdown text={content} />
        </div>
      ) : (
        <Textarea
          class="document-input"
          aria-label="Document content"
          value={content}
          readOnly={!item.writable}
          spellcheck={false}
          onInput={(event) => setContent(event.currentTarget.value)}
        />
      )}
      {error && !dialog && <LoadError error={error} />}
      <details>
        <summary>Details</summary>
        <dl class="document-details">
          <dt>Path</dt>
          <dd>{item.path || "Created when saved"}</dd>
          <dt>Updated</dt>
          <dd>{item.modified ? <Time value={item.modified} /> : "—"}</dd>
          <dt>Origin</dt>
          <dd>
            {item.provenance
              ? item.provenance.automatic
                ? `Automatic memory · conversation ${item.provenance.source_session}`
                : "Explicit save"
              : "No recorded provenance"}
          </dd>
          <dt>Context</dt>
          <dd>
            {!memory
              ? "Loaded when invoked. Catalogue changes apply to new conversations."
              : item.scope === "global" && item.source === "uAgent"
                ? "Eligible for startup context, within the configured memory limit."
                : "Available for retrieval when needed."}
          </dd>
          {item.required_tools?.length ? (
            <>
              <dt>Required tools</dt>
              <dd>{item.required_tools.join(", ")}</dd>
            </>
          ) : null}
          {item.files?.length ? (
            <>
              <dt>Files</dt>
              <dd>{item.files.join("\n")}</dd>
            </>
          ) : null}
        </dl>
      </details>
      {item.writable && editing && (
        <div class="editor-actions">
          <small class="muted">Saved changes apply to new conversations.</small>
          <Button
            onClick={() => {
              forget();
              if (!item.key) close();
              setContent(item.content || "");
              setEditing(false);
            }}
          >
            Cancel
          </Button>
          <Button
            variant="primary"
            disabled={
              !online ||
              !dirty ||
              !content.trim() ||
              (!item.key && !name.trim()) ||
              stale
            }
            busy={busy}
            onClick={save}
          >
            Save
          </Button>
        </div>
      )}
      {dialog === "delete" && (
        <ConfirmModal
          title={`Delete ${item.name}?`}
          action="Delete"
          busy={busy}
          error={error}
          confirm={() =>
            online &&
            run(async () => {
              await call("forget");
              forget();
              close();
            })
          }
          close={() => setDialog(null)}
        >
          Delete this {memory ? "memory" : "skill manifest"}?
          {!memory && " Supporting files are kept."}
        </ConfirmModal>
      )}
      {(dialog === "rename" || dialog === "move") && (
        <Modal
          title={dialog === "rename" ? "Rename memory" : `Move ${item.name}`}
          close={() => setDialog(null)}
        >
          <form
            onSubmit={(event) => {
              event.preventDefault();
              if (busy || !online) return;
              if (dialog === "move") move(target, false);
              else
                run(() =>
                  call("rename", { target: `${item.scope}/${target}` }).then(
                    (result) => show(result.item),
                  ),
                );
            }}
          >
            {dialog === "rename" ? (
              <Field label="Name">
                <Input
                  autoFocus
                  value={target}
                  onInput={(event) => setTarget(event.currentTarget.value)}
                />
              </Field>
            ) : (
              <WhereField
                value={target}
                places={places.filter(
                  (place) => item.scope !== "project" || place.path !== cwd,
                )}
                everywhere={item.scope === "project"}
                change={setTarget}
              />
            )}
            {error && <LoadError error={error} />}
            <Actions>
              <Button onClick={() => setDialog(null)}>Cancel</Button>
              <Button
                type="submit"
                variant="primary"
                disabled={busy || !online}
              >
                {dialog === "rename" ? "Rename" : "Move"}
              </Button>
            </Actions>
          </form>
        </Modal>
      )}
    </fieldset>
  );
}
