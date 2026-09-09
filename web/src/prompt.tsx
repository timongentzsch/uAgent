import { useEffect, useRef, useState } from "preact/hooks";
import type {
  CommandFields,
  PromptDocument,
  PromptResult,
  Session,
} from "./types.ts";
import { command, readStored, writeStored } from "./store.ts";
import { Field, Select, Skeleton, LoadError } from "./ui.tsx";
import { ProjectField } from "./management.tsx";
import "./prompt.css";

export default function PromptEditor({
  session,
  projects,
  online,
  version,
  scope: initialScope,
  edit = false,
}: {
  session?: Session;
  projects: string[];
  online: boolean;
  version: number;
  scope?: string;
  edit?: boolean;
}) {
  const active =
    session?.generation &&
    !["saved", "draft", "closed", "starting"].includes(session.status || "")
      ? session
      : undefined;
  const [scope, setScope] = useState(
    initialScope ||
      (active && !active.turn_active
        ? "conversation"
        : session
          ? "project"
          : "global"),
  );
  const [cwd, setCwd] = useState(session?.cwd || projects[0] || "");
  const [loaded, setLoaded] = useState<{ key: string; value: PromptResult }>();
  const [draft, setDraft] = useState<PromptDocument | null>(null);
  const [preview, setPreview] = useState<PromptResult>();
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const [pane, setPane] = useState(edit ? "instructions" : "effective");
  const [attempt, setAttempt] = useState(0);
  const sequence = useRef(0);
  const openEditor = useRef(edit);
  const target =
    active?.cwd === cwd && !active?.turn_active ? active : undefined;
  const key = `uagent-prompt-${scope}-${scope === "global" ? "" : scope === "conversation" ? target?.id : cwd}`;
  const data = loaded?.key === key ? loaded.value : undefined;
  const storage = sessionStorage;
  const request = async (fields: CommandFields) => {
    const result = await command("prompt", target || null, {
      scope,
      cwd,
      ...fields,
    });
    if (result.pending)
      throw new Error(
        "Prompt operation is pending. Reload to inspect its result.",
      );
    return result.result;
  };
  useEffect(() => {
    setDraft(readStored<PromptDocument | null>(storage, key, null));
    setPreview(undefined);
  }, [key]);
  useEffect(() => {
    const current = ++sequence.current;
    setError(null);
    if (online)
      request({ action: "show" })
        .then((value) => {
          if (current !== sequence.current) return;
          setLoaded({ key, value });
          if (openEditor.current) {
            openEditor.current = false;
            setDraft((prior) => prior || editable(value));
          }
        })
        .catch((failure) => {
          if (current === sequence.current) setError(failure);
        });
    return () => {
      ++sequence.current;
    };
  }, [key, target?.generation, online, version, attempt]);
  function editable(value: PromptResult): PromptDocument {
    return value.item.mode === "inherit"
      ? { ...value.item, mode: "replace", text: value.inherited[scope] || "" }
      : value.item;
  }
  function update(value: PromptDocument | null) {
    setDraft(value);
    setPreview(undefined);
    writeStored(storage, key, value);
  }
  async function perform(action: "preview" | "set" | "reset") {
    if (!data) return;
    const selected = draft || data.item;
    setBusy(true);
    setError(null);
    try {
      const value = await request({
        action,
        revision: selected.revision,
        mode: selected.mode,
        text: selected.text,
      });
      if (action === "preview") {
        setPreview(value);
        setPane("effective");
      } else {
        setLoaded({ key, value });
        update(null);
      }
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  }
  const shown = preview || data;
  return (
    <div class="prompt-editor">
      <div class="prompt-controls">
        <Field label="Scope">
          <Select
            aria-label="Prompt scope"
            value={scope}
            disabled={busy}
            onChange={(event) => {
              const value = event.currentTarget.value;
              setScope(value);
              if (value === "conversation") setCwd(session?.cwd || "");
            }}
          >
            <option value="global">Global</option>
            <option value="project">Project</option>
            {active && (
              <option value="conversation" disabled={active.turn_active}>
                This conversation
              </option>
            )}
          </Select>
        </Field>
        {scope === "project" && (
          <ProjectField value={cwd} projects={projects} change={setCwd} />
        )}
      </div>
      {error && (
        <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
      )}
      {!data ? (
        !error && <Skeleton rows={12} label="Loading system prompt…" />
      ) : (
        <>
          <p class="muted">
            {data.preview_kind ||
              "Next request · runtime and repository context included."}
          </p>
          {draft && draft.revision !== data.item.revision && (
            <p role="status">
              Changed elsewhere. Your draft is retained; reload and compare
              before saving.
              <button
                onClick={() =>
                  update({ ...draft, revision: data.item.revision })
                }
              >
                Use my draft against the latest version
              </button>
            </p>
          )}
          <div class="prompt-tabs segmented" aria-label="Prompt view">
            {["instructions", "effective"].map((name) => (
              <button
                aria-pressed={pane === name}
                onClick={() => setPane(name)}
              >
                {name === "instructions" ? "Instructions" : "Effective prompt"}
              </button>
            ))}
          </div>
          <div class={`prompt-columns ${pane}`}>
            <section>
              <h3>
                {scope === "conversation"
                  ? "This conversation"
                  : scope === "global"
                    ? "Global"
                    : "Project"}{" "}
                instructions
              </h3>
              {draft ? (
                <>
                  <Field label="Mode">
                    <Select
                      aria-label="Prompt mode"
                      value={draft.mode}
                      disabled={busy}
                      onChange={(event) =>
                        update({
                          ...draft,
                          mode: event.currentTarget
                            .value as PromptDocument["mode"],
                        })
                      }
                    >
                      <option value="inherit">Inherit</option>
                      <option value="overlay">
                        Overlay · add instructions
                      </option>
                      <option value="replace">Replace inherited prompt</option>
                    </Select>
                  </Field>
                  <textarea
                    aria-label="System prompt text"
                    value={draft.text}
                    disabled={busy || draft.mode === "inherit"}
                    onInput={(event) =>
                      update({ ...draft, text: event.currentTarget.value })
                    }
                  />
                </>
              ) : (
                <>
                  <small class="muted">
                    {data.item.mode}
                    {data.item.path && ` · ${data.item.path}`}
                  </small>
                  <pre>
                    {data.item.mode === "inherit"
                      ? "Using inherited instructions."
                      : data.item.text || "Empty replacement."}
                  </pre>
                </>
              )}
              <div class="dialog-actions">
                {draft ? (
                  <>
                    <button
                      disabled={!online || busy}
                      onClick={() => perform("preview")}
                    >
                      Preview changes
                    </button>
                    <button
                      disabled={!online || busy}
                      onClick={() => perform("set")}
                    >
                      Save
                    </button>
                    <button disabled={busy} onClick={() => update(null)}>
                      Discard edit
                    </button>
                  </>
                ) : (
                  <button
                    disabled={!online || busy}
                    onClick={() => update(editable(data))}
                  >
                    {data.item.mode === "inherit"
                      ? "Edit inherited prompt"
                      : "Edit"}
                  </button>
                )}
                {data.item.mode !== "inherit" && (
                  <button
                    disabled={!online || busy}
                    onClick={() => perform("reset")}
                  >
                    Reset to inherited
                  </button>
                )}
              </div>
              <small class="muted">
                Saves apply to the next model request. Unsent edits stay in this
                browser tab.
              </small>
            </section>
            <section>
              <h3>
                {preview ? "Proposed effective prompt" : "Effective prompt"}
              </h3>
              {shown && (
                <>
                  <div class="prompt-sources">
                    {shown.sources.map((source) => (
                      <span class={source.active ? "" : "muted"}>
                        {source.scope}
                        {source.active
                          ? ""
                          : source.mode === "inherit"
                            ? " · inherit"
                            : " · overridden"}
                      </span>
                    ))}
                  </div>
                  {preview?.diff && (
                    <details open>
                      <summary>Changes</summary>
                      <pre>{preview.diff}</pre>
                    </details>
                  )}
                  <pre aria-label="Effective system prompt">
                    {shown.effective}
                  </pre>
                  {data.last_sent && data.last_sent !== shown.effective && (
                    <details>
                      <summary>Last sent prompt</summary>
                      <pre>{data.last_sent}</pre>
                    </details>
                  )}
                </>
              )}
            </section>
          </div>
        </>
      )}
    </div>
  );
}
