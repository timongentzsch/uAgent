import type { LibraryItem } from "./types.ts";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { Plus, ArrowLeft, ChevronDown, Pencil } from "lucide-preact";
import { Field, LoadError, Modal, Select, Skeleton } from "./ui.tsx";
import { Menu, MenuItem } from "./popover.tsx";
import { readStored, writeStored } from "./store.ts";
import { bytes } from "./quantities.ts";
import {
  manage,
  useLibrary,
  ProjectField,
  ScopeField,
  dateTime,
} from "./management.tsx";
import Markdown from "./markdown-view.tsx";
import FolderLabel, { folderName } from "./folder-label.tsx";
import "./message.css";

type Draft = {
  item: LibraryItem;
  content: string;
  name: string;
  scope: string;
};
export default function Library({
  projects,
  cwd: initial,
  online,
  version,
}: {
  projects: string[];
  cwd: string;
  online: boolean;
  version: number;
}) {
  const [cwd, setCwd] = useState(initial || projects[0] || "");
  const [visited, setVisited] = useState([initial]);
  const [expanded, setExpanded] = useState(true);
  const folders = [...new Set([...projects, ...visited, cwd].filter(Boolean))];
  function chooseProject(path: string) {
    setVisited((paths) => (paths.includes(path) ? paths : [...paths, path]));
    setCwd(path);
    setExpanded(true);
    if (scope.startsWith("project:")) setScope(`project:${path}`);
  }
  const [kind, setKind] = useState<"memory" | "skills">("memory");
  const [scope, setScope] = useState("all");
  const projectFilter = scope.startsWith("project:") ? scope.slice(8) : "";
  const [query, setQuery] = useState("");
  const {
    data,
    error: listError,
    refresh,
  } = useLibrary(kind, cwd, version, online);
  const [item, setItem] = useState<LibraryItem | null>(null);
  const [content, setContent] = useState("");
  const [name, setName] = useState("");
  const [newScope, setNewScope] = useState("project");
  const [loading, setLoading] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [editing, setEditing] = useState(false);
  const [dialog, setDialog] = useState<"delete" | "rename" | null>(null);
  const [rename, setRename] = useState("");
  const generation = useRef(0);
  const storage = `uagent-library-${kind}-${cwd}`;
  const dirty = !!item && (!item.key || content !== item.content);
  useLayoutEffect(() => {
    generation.current++;
    setError(null);
    setLoading(false);
    const key = sessionStorage.getItem(`${storage}-last`);
    const saved = key
      ? readStored<Draft | null>(sessionStorage, key, null)
      : null;
    setItem(saved?.item || null);
    setContent(saved?.content || "");
    setName(saved?.name || "");
    setNewScope(saved?.scope || "project");
    setEditing(!!saved);
  }, [kind, cwd]);
  useEffect(() => {
    if (!item || !dirty) return;
    const key = `${storage}-${item.key || "new"}`;
    writeStored(sessionStorage, key, { item, content, name, scope: newScope });
    sessionStorage.setItem(`${storage}-last`, key);
  }, [item, content, name, newScope]);
  function clearDraft() {
    sessionStorage.removeItem(`${storage}-${item?.key || "new"}`);
    sessionStorage.removeItem(`${storage}-last`);
  }
  useEffect(
    () => () => {
      generation.current++;
    },
    [],
  );
  async function select(entry: LibraryItem) {
    const current = ++generation.current;
    setLoading(true);
    setError(null);
    try {
      const result = await manage(kind, { cwd, action: "get", key: entry.key });
      if (generation.current !== current || !result.item) return;
      if (result.item.error) throw new Error(result.item.error);
      const saved = readStored<Draft | null>(
        sessionStorage,
        `${storage}-${entry.key}`,
        null,
      );
      setItem(saved?.item || result.item);
      setName(result.item.name);
      setNewScope(result.item.scope);
      setContent(saved?.content ?? result.item.content ?? "");
      setEditing(!!saved);
    } catch (failure) {
      if (generation.current === current) setError(failure);
    } finally {
      if (generation.current === current) setLoading(false);
    }
  }
  function create(copy = false) {
    generation.current++;
    setLoading(false);
    setError(null);
    setEditing(true);
    setExpanded(true);
    setName(copy && item ? `${item.name}-copy` : "");
    setNewScope(scope === "global" ? "global" : "project");
    const text = copy
      ? content
      : kind === "skills"
        ? "---\ndescription: Describe when to use this skill\n---\n\n"
        : "";
    setItem({
      key: "",
      name: "",
      path: "",
      scope: "project",
      source: "uAgent",
      writable: true,
      revision: "",
      content: "",
    });
    setContent(text);
  }
  async function mutate(action: string, target?: string) {
    if (!item) return;
    const current = generation.current;
    setBusy(true);
    setError(null);
    try {
      const result = await manage(kind, {
        cwd,
        action,
        key: item.key || `${newScope}/${name}`,
        revision: item.revision,
        ...(action === "set" ? { content } : {}),
        ...(target ? { target } : {}),
      });
      if (action === "set" || action === "forget") clearDraft();
      if (generation.current !== current) return;
      setDialog(null);
      if (action === "forget") setItem(null);
      else if (result.item) {
        setItem(result.item);
        setContent(result.item.content || "");
        if (action === "set") setEditing(false);
      }
      refresh();
    } catch (failure) {
      if (generation.current === current) setError(failure);
    } finally {
      setBusy(false);
    }
  }
  const current = data?.items?.find((entry) => entry.key === item?.key);
  const changed = !!(
    item?.key &&
    data &&
    (!current || item.revision !== current.revision)
  );
  const items = (data?.items || []).filter(
    (entry) =>
      (scope === "all" ||
        entry.scope === (projectFilter ? "project" : scope)) &&
      `${entry.name} ${entry.description || ""}`
        .toLowerCase()
        .includes(query.toLowerCase()),
  );
  function rows(group?: string) {
    if (listError) return null;
    if (!data) return <Skeleton rows={3} label="Loading library…" />;
    const entries = group
      ? items.filter((entry) => entry.scope === group)
      : items;
    return entries.length ? (
      entries.map((entry) => (
        <button
          key={entry.key}
          class={`library-row ${item?.key === entry.key ? "selected" : ""}`}
          disabled={busy}
          onClick={() => select(entry)}
        >
          <span>{entry.name}</span>
          <small>
            {group ? entry.source : `${entry.scope} · ${entry.source}`}
            {entry.status && entry.status !== "available"
              ? ` · ${entry.status}`
              : ""}
          </small>
        </button>
      ))
    ) : (
      <p class="muted">No {kind === "memory" ? "memories" : "skills"} found.</p>
    );
  }
  return (
    <div class="management">
      <div class="management-toolbar">
        <div class="segmented" aria-label="Library type">
          {(
            [
              ["memory", "Memories"],
              ["skills", "Skills"],
            ] as const
          ).map(([value, label]) => (
            <button
              disabled={busy}
              aria-pressed={kind === value}
              onClick={() => {
                setKind(value);
                if (value === "skills" && projectFilter) setScope("project");
              }}
            >
              {label}
            </button>
          ))}
        </div>
        <ProjectField value={cwd} projects={folders} change={chooseProject} />
        <button
          class="primary with-icon"
          disabled={!online || !cwd || busy}
          onClick={() => create()}
        >
          <Plus />
          Add {kind === "memory" ? "memory" : "skill"}
        </button>
      </div>
      {kind === "memory" && data?.enabled === false && (
        <p class="muted">
          Memory is disabled in configuration. Saved entries remain available to
          manage.
        </p>
      )}
      {!cwd ? (
        <div class="empty">
          <p>Choose a project to browse its library and your global items.</p>
        </div>
      ) : (
        <div
          class={`management-body ${item || loading ? "has-selection" : ""}`}
        >
          <div class="management-list">
            <input
              aria-label="Search library"
              type="search"
              placeholder="Search…"
              value={query}
              onInput={(event) => setQuery(event.currentTarget.value)}
            />
            <Select
              aria-label="Filter library"
              value={scope}
              onChange={(event) => {
                const value = event.currentTarget.value;
                setScope(value);
                if (value.startsWith("project:")) chooseProject(value.slice(8));
              }}
            >
              <option value="all">All scopes</option>
              <option value="project">Project</option>
              <option value="global">Global</option>
              {kind === "memory" && (
                <optgroup label="Projects">
                  {folders.map((path) => (
                    <option key={path} value={`project:${path}`} title={path}>
                      {folderName(path)}
                    </option>
                  ))}
                </optgroup>
              )}
            </Select>
            {listError && <LoadError error={listError} retry={refresh} />}
            {kind === "memory" ? (
              <>
                {scope !== "project" && !projectFilter && (
                  <section class="library-group" aria-label="Global memories">
                    <h2>
                      <FolderLabel>Global</FolderLabel>
                    </h2>
                    {rows("global")}
                  </section>
                )}
                {scope !== "global" &&
                  folders
                    .filter((path) => !projectFilter || path === projectFilter)
                    .map((path) => (
                      <section
                        key={path}
                        class="library-group"
                        aria-label={path}
                      >
                        <h2>
                          <button
                            disabled={busy}
                            aria-expanded={path === cwd && expanded}
                            onClick={() =>
                              path === cwd
                                ? setExpanded(!expanded)
                                : chooseProject(path)
                            }
                          >
                            <FolderLabel path={path} />
                            <ChevronDown aria-hidden="true" />
                          </button>
                        </h2>
                        {path === cwd && expanded && rows("project")}
                      </section>
                    ))}
              </>
            ) : (
              rows()
            )}
          </div>
          <fieldset disabled={busy} class="management-editor">
            {loading ? (
              <Skeleton rows={12} label="Loading document…" />
            ) : item ? (
              <>
                <div class="editor-head">
                  <button class="quiet with-icon" onClick={() => setItem(null)}>
                    <ArrowLeft />
                    Back
                  </button>
                  <strong>
                    {item.key
                      ? item.name
                      : `New ${kind === "memory" ? "memory" : "skill"}`}
                  </strong>
                  {item.key && (
                    <Menu label="Library item menu">
                      <MenuItem onClick={() => create(true)}>
                        {kind === "skills"
                          ? "Copy instructions to…"
                          : "Copy to…"}
                      </MenuItem>
                      {kind === "skills" && (
                        <MenuItem
                          disabled={!online || busy}
                          onClick={() =>
                            mutate(
                              (current?.status || item.status) === "disabled"
                                ? "enable"
                                : "disable",
                            )
                          }
                        >
                          {(current?.status || item.status) === "disabled"
                            ? "Enable globally"
                            : "Disable globally"}
                        </MenuItem>
                      )}
                      {item.writable && kind === "memory" && (
                        <MenuItem
                          onClick={() => {
                            setRename(item.name);
                            setDialog("rename");
                          }}
                        >
                          Rename
                        </MenuItem>
                      )}
                      {item.writable && (
                        <MenuItem onClick={() => setDialog("delete")}>
                          Delete
                        </MenuItem>
                      )}
                    </Menu>
                  )}
                </div>
                {!item.key ? (
                  <div class="field-row">
                    <Field label="Name">
                      <input
                        value={name}
                        onInput={(event) => setName(event.currentTarget.value)}
                      />
                    </Field>
                    <ScopeField value={newScope} change={setNewScope} />
                  </div>
                ) : (
                  <p class="muted">
                    {item.scope} · {item.source}
                    {!item.writable ? " · Read-only" : ""}
                  </p>
                )}
                {changed && (
                  <p role="status">
                    Changed on disk. Your draft is retained.{" "}
                    <button
                      onClick={() => {
                        clearDraft();
                        if (current) select(current);
                        else setItem(null);
                      }}
                    >
                      Reload
                    </button>
                  </p>
                )}
                <div class="document-toolbar">
                  {item.writable && !editing && (
                    <button class="with-icon" onClick={() => setEditing(true)}>
                      <Pencil />
                      Edit
                    </button>
                  )}
                  <small>
                    {bytes(new TextEncoder().encode(content).length)}
                    {item.writable && data?.limit
                      ? ` / ${bytes(data.limit)}`
                      : ""}
                  </small>
                </div>
                {!editing ? (
                  <div class="document-preview">
                    <Markdown text={content} />
                  </div>
                ) : (
                  <textarea
                    class="document-input"
                    aria-label="Document content"
                    value={content}
                    readOnly={!item.writable}
                    spellcheck={false}
                    onInput={(event) => setContent(event.currentTarget.value)}
                  />
                )}
                {error && <LoadError error={error} />}
                <details>
                  <summary>Details</summary>
                  <dl class="document-details">
                    <dt>Path</dt>
                    <dd>{item.path || "Created when saved"}</dd>
                    <dt>Updated</dt>
                    <dd>
                      {dateTime(
                        item.modified ? item.modified / 1000 : undefined,
                      )}
                    </dd>
                    <dt>Origin</dt>
                    <dd>
                      {item.provenance
                        ? item.provenance.automatic
                          ? `Automatic memory · session ${item.provenance.source_session}`
                          : "Explicit save"
                        : "No recorded provenance"}
                    </dd>
                    <dt>Context</dt>
                    <dd>
                      {kind === "skills"
                        ? "Loaded when invoked. Catalogue changes apply to new sessions."
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
                    <small class="muted">
                      Saved changes apply to new sessions.
                    </small>
                    <button
                      onClick={() => {
                        setContent(item.content || "");
                        clearDraft();
                        setEditing(false);
                        if (!item.key) setItem(null);
                      }}
                    >
                      Cancel
                    </button>
                    <button
                      class="primary"
                      disabled={
                        !online ||
                        busy ||
                        !dirty ||
                        !content.trim() ||
                        (!item.key && !name.trim()) ||
                        changed
                      }
                      onClick={() => mutate("set")}
                    >
                      {busy ? "Saving…" : "Save"}
                    </button>
                  </div>
                )}
              </>
            ) : (
              <div class="empty">
                <p>Select an item to view or edit it.</p>
                {error && <LoadError error={error} />}
              </div>
            )}
          </fieldset>
        </div>
      )}
      {dialog && item && (
        <Modal
          title={dialog === "delete" ? `Delete ${item.name}?` : "Rename memory"}
          close={() => setDialog(null)}
        >
          {dialog === "delete" ? (
            <p>
              Delete this {item.scope}{" "}
              {kind === "memory" ? "memory" : "skill manifest"}?
              {kind === "skills" && " Supporting files are kept."}
            </p>
          ) : (
            <Field label="Name">
              <input
                value={rename}
                onInput={(event) => setRename(event.currentTarget.value)}
              />
            </Field>
          )}
          {error && <LoadError error={error} />}
          <div class="dialog-actions">
            <button onClick={() => setDialog(null)}>Cancel</button>
            <button
              class="primary"
              disabled={busy || !online}
              onClick={() =>
                mutate(
                  dialog === "delete" ? "forget" : "rename",
                  dialog === "rename" ? `${item.scope}/${rename}` : undefined,
                )
              }
            >
              {dialog === "delete" ? "Delete" : "Rename"}
            </button>
          </div>
        </Modal>
      )}
    </div>
  );
}
