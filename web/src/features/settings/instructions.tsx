import { useRef, useState } from "preact/hooks";
import { useAction } from "../../shared/use-action.ts";
import { useResource } from "../../shared/use-resource.ts";
import type {
  InstructionFile,
  InstructionStack,
  SelfDirective,
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
import { ProjectField } from "../library/management.tsx";
import "./instructions.css";

const TITLES: Record<string, string> = {
  "sessions/user": "Yours · every conversation",
  "sessions/project": "Project · every conversation",
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
  // Tagged with its project, so another project's files never show, or get
  // saved, under this one.
  const {
    value: loaded,
    error,
    retry,
    setValue: setLoaded,
  } = useResource<{ cwd: string; stack: InstructionStack }>(
    () =>
      online
        ? command("instructions", null, { action: "show", cwd }).then(
            (value) => {
              if (value.pending)
                throw new Error("Reload to see the instructions.");
              return { cwd, stack: value.result };
            },
          )
        : undefined,
    [cwd, online, version],
  );
  const stack = loaded?.cwd === cwd ? loaded.stack : undefined;
  const self = state?.self_directive;
  const card = (file: InstructionFile) => (
    <InstructionCard
      key={file.path}
      file={file}
      cwd={cwd}
      online={online}
      saved={(next) =>
        setLoaded((prior) =>
          prior?.cwd === cwd ? { cwd, stack: next } : prior,
        )
      }
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
        <LoadError error={error} retry={retry} />
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
            <h3>Chat members</h3>
            <Base text={stack?.base.member} label="Built-in member base" />
            <p class="muted instructions-also">
              A coordinator fills in each member&rsquo;s name, persona and
              skills when it adds one. What a member was sent is under
              &ldquo;Show prompt&rdquo; on its messages.
            </p>
          </>
        </Placeholder>
      )}
      {self && session && (
        <SelfDirectiveCard self={self} session={session} online={online} />
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
    <details>
      <summary>{label}</summary>
      <pre>{text}</pre>
    </details>
  );
}

// The agent's own directive for this conversation; only clearing it is
// yours.
function SelfDirectiveCard({
  self,
  session,
  online,
}: {
  self: SelfDirective;
  session: Session;
  online: boolean;
}) {
  const [error, setError] = useState<unknown>(null);
  return (
    <section class="instruction-card">
      <header>
        <strong>Self-directive · this conversation</strong>
        <small class="muted">{self.mode}, written by the agent</small>
      </header>
      {error && <LoadError error={error} />}
      <pre>{self.text}</pre>
      <Actions>
        <Button
          disabled={!online}
          onClick={() => {
            setError(null);
            command("self_directive", session, {
              action: "reset",
              revision: self.revision,
            }).catch(setError);
          }}
        >
          Clear
        </Button>
      </Actions>
    </section>
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
  // Null until edited: the field shows the file as it is, including a newer
  // version loaded meanwhile, and an edit in progress is never replaced. An
  // edit remembers the text it started from, so saving over a file changed
  // meanwhile (another tab, the agent) is refused rather than lost.
  const [draft, setDraft] = useState<{ text: string; base: string } | null>(
    null,
  );
  const { run, busy, error } = useAction();
  const field = useRef<HTMLTextAreaElement>(null);
  const text = draft?.text ?? file.text;
  const changed = text !== file.text;
  async function save() {
    await run(async () => {
      const value = await command("instructions", null, {
        action: "set",
        cwd,
        audience: file.audience,
        scope: file.scope,
        text,
        base: draft?.base ?? file.text,
      });
      if (!value.pending) {
        saved(value.result);
        setDraft(null);
      }
    });
    // Save leaves with the edit; focus stays in the field, not the page.
    field.current?.focus();
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
        inputRef={field}
        grow
        value={text}
        placeholder="Nothing yet."
        readOnly={busy}
        disabled={!online}
        onInput={(event) =>
          setDraft({
            text: event.currentTarget.value,
            base: draft?.base ?? file.text,
          })
        }
      />
      {changed && (
        <Actions>
          <Button
            disabled={busy}
            onClick={() => {
              setDraft(null);
              field.current?.focus();
            }}
          >
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
