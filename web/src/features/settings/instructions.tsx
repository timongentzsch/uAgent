import { useEffect, useState } from "preact/hooks";
import type {
  InstructionFile,
  InstructionStack,
  Session,
  State,
} from "../../shared/types.ts";
import { command } from "../../state/api.ts";
import {
  Actions,
  Button,
  LoadError,
  Placeholder,
  Textarea,
} from "../../shared/ui.tsx";
import { ProjectField } from "./management.tsx";
import "./instructions.css";

const TITLES: Record<string, string> = {
  "sessions/user": "Yours · every session",
  "sessions/project": "Project · every session",
  "coordinator/user": "Yours · coordinator",
  "coordinator/project": "Project · coordinator",
};

// Everything a session reads before your first message, top to bottom in the
// order it reads it. Files only add to the base; edits reach new and
// restarted sessions, so the running ones keep their cached prefix.
export default function Instructions({
  session,
  state,
  projects,
  online,
  version,
  showRequest,
}: {
  session?: Session;
  state?: State;
  projects: string[];
  online: boolean;
  version: number;
  showRequest?: () => void;
}) {
  const [cwd, setCwd] = useState(session?.cwd || projects[0] || "");
  const [stack, setStack] = useState<InstructionStack>();
  const [error, setError] = useState<unknown>(null);
  const load = () =>
    command("instructions", null, { action: "show", cwd }).then((value) => {
      if (value.pending) throw new Error("Reload to see the instructions.");
      setStack(value.result);
    });
  useEffect(() => {
    setError(null);
    if (online) load().catch(setError);
  }, [cwd, online, version]);
  const self = state?.self_directive;
  const card = (file: InstructionFile) => (
    <InstructionCard
      key={file.path}
      file={file}
      cwd={cwd}
      online={online}
      saved={setStack}
    />
  );
  const files = stack?.files || [];
  return (
    <div class="instructions">
      <ProjectField value={cwd} projects={projects} change={setCwd} />
      <p class="muted">
        Read in this order when a session starts, after the built-in base.
        Changes reach new and restarted sessions.
      </p>
      {error ? (
        <LoadError error={error} retry={() => load().catch(setError)} />
      ) : (
        <Placeholder label="Loading instructions…" when={!stack}>
          <>
            <Base text={stack?.base.sessions} label="Built-in base" />
            {files.filter((file) => file.audience === "sessions").map(card)}
            {!!stack?.also_loaded.length && (
              <p class="muted instructions-also">
                Also read here: {stack.also_loaded.join(", ")}
              </p>
            )}
            <h3>Coordinator</h3>
            <Base
              text={stack?.base.coordinator}
              label="Built-in coordinator base"
            />
            {files.filter((file) => file.audience === "coordinator").map(card)}
          </>
        </Placeholder>
      )}
      {self && session && (
        <section class="instruction-card">
          <header>
            <strong>Self-directive · this conversation</strong>
            <small class="muted">{self.mode}, written by the agent</small>
          </header>
          <pre>{self.text}</pre>
          <Actions>
            <Button
              disabled={!online}
              onClick={() =>
                command("self_directive", session, {
                  action: "reset",
                  revision: self.revision,
                }).catch(setError)
              }
            >
              Clear
            </Button>
          </Actions>
        </section>
      )}
      {showRequest && (
        <Actions>
          <Button variant="quiet" onClick={showRequest}>
            Preview this conversation's request
          </Button>
        </Actions>
      )}
    </div>
  );
}

function Base({ text, label }: { text?: string; label: string }) {
  return (
    <details class="instructions-base">
      <summary>{label}</summary>
      <pre>{text}</pre>
    </details>
  );
}

function InstructionCard({
  file,
  cwd,
  online,
  saved,
}: {
  file: InstructionFile;
  cwd: string;
  online: boolean;
  saved: (stack: InstructionStack) => void;
}) {
  const [draft, setDraft] = useState(file.text);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  useEffect(() => setDraft(file.text), [file.text]);
  const changed = draft !== file.text;
  async function save() {
    setBusy(true);
    setError(null);
    try {
      const value = await command("instructions", null, {
        action: "set",
        cwd,
        audience: file.audience,
        scope: file.scope,
        text: draft,
      });
      if (!value.pending) saved(value.result);
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  }
  const title = TITLES[`${file.audience}/${file.scope}`];
  return (
    <section class="instruction-card">
      <header>
        <strong>{title}</strong>
        <small class="muted" title={file.path}>
          <span dir="ltr">{file.path}</span>
        </small>
      </header>
      {error && <LoadError error={error} />}
      <Textarea
        aria-label={title}
        grow
        value={draft}
        placeholder="Nothing yet."
        disabled={busy || !online}
        onInput={(event) => setDraft(event.currentTarget.value)}
      />
      {changed && (
        <Actions>
          <Button disabled={busy} onClick={() => setDraft(file.text)}>
            Discard
          </Button>
          <Button variant="primary" disabled={busy || !online} onClick={save}>
            Save
          </Button>
        </Actions>
      )}
    </section>
  );
}
