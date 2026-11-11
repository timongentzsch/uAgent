import { useRef, useState } from "preact/hooks";
import { useAction } from "../../shared/use-action.ts";
import { useResource } from "../../shared/use-resource.ts";
import type {
  InstructionFile,
  InstructionStack,
  Session,
} from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
import {
  Actions,
  Button,
  LoadError,
  Placeholder,
  Textarea,
} from "../../shared/ui.tsx";
import { ProjectField } from "../../shared/project-field.tsx";
import "./instructions.css";

// What a conversation reads at start, in reading order and edited in place:
// the system message's sections, then the instruction files (yours and the
// folder's).

// What the saved system message file holds: the sections replaced and the
// text added after the last (docs/INSTRUCTIONS.md).
interface PromptChanges {
  replace?: Record<string, string>;
  append?: string;
}

const TITLES: Record<string, string> = {
  user: "Yours · every conversation",
  project: "Folder · every conversation",
};

// Everything a session reads before your first message, top to bottom in the
// order it reads it. Files only add to the system message; edits reach new
// and restarted sessions, so the running ones keep their cached prefix.
export default function Instructions({
  session,
  projects,
  online,
  version,
  showRequest,
}: {
  session?: Session;
  projects: string[];
  online: boolean;
  version: number;
  showRequest?: () => void;
}) {
  const { command } = useApi();
  const [cwd, setCwd] = useState(session?.cwd || projects[0] || "");
  // Tagged with its folder, so another folder's files never show, or get
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
  // Saves `text` over what was read as `base`; false while it awaits a
  // person elsewhere.
  const set = async (scope: string, text: string, base: string) => {
    const value = await command("instructions", null, {
      action: "set",
      cwd,
      scope,
      text,
      base,
    });
    if (value.pending) return false;
    setLoaded((prior) =>
      prior?.cwd === cwd ? { cwd, stack: value.result } : prior,
    );
    return true;
  };
  const card = (file: InstructionFile) => (
    <EditCard
      key={file.path}
      title={TITLES[file.scope]}
      note={file.path}
      text={file.text}
      online={online}
      save={(text, base) => set(file.scope, text, base)}
    />
  );
  const files = stack?.files || [];
  return (
    <div class="instructions">
      <ProjectField value={cwd} projects={projects} change={setCwd} />
      <p class="muted">
        Read in this order when a conversation starts. Changes reach new and
        restarted conversations.
      </p>
      {error ? (
        <LoadError error={error} retry={retry} />
      ) : (
        <Placeholder label="Loading instructions…" when={!stack}>
          <>
            {stack && (
              <SystemMessage
                prompt={stack.prompt}
                online={online}
                save={(text) => set("prompt", text, stack.prompt.saved)}
              />
            )}
            {files.map(card)}
            {!!stack?.also_loaded.length && (
              <p class="muted instructions-also">
                Also read here: {stack.also_loaded.join(", ")}
              </p>
            )}
          </>
        </Placeholder>
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

// The system message's sections, each with its built-in text or yours. A
// changed one says so, and Reset brings the built-in text back; what you add
// after the last section is yours alone.
function SystemMessage({
  prompt,
  online,
  save,
}: {
  prompt: InstructionStack["prompt"];
  online: boolean;
  save: (text: string) => Promise<boolean>;
}) {
  let mine: PromptChanges = {};
  try {
    mine = prompt.saved ? JSON.parse(prompt.saved) : {};
  } catch {
    // A file edited by hand into something unreadable changes nothing.
  }
  const builtIn = Object.fromEntries(
    prompt.sections.map((section) => [section.name, section.text]),
  );
  // The file holds only what differs: a section put back to its built-in
  // text leaves it, and with nothing left the file goes.
  const write = (replace: Record<string, string>, append: string) => {
    const changed = Object.fromEntries(
      Object.entries(replace).filter(([name, text]) => text !== builtIn[name]),
    );
    const next: PromptChanges = {
      ...(Object.keys(changed).length && { replace: changed }),
      ...(append && { append }),
    };
    return save(Object.keys(next).length ? JSON.stringify(next, null, 2) : "");
  };
  const replace = mine.replace ?? {};
  const changes = Object.keys(replace).length + (mine.append ? 1 : 0);
  return (
    <details>
      <summary>System message{changes ? ` · ${changes} changed` : ""}</summary>
      <div class="instructions">
        {prompt.sections.map(({ name, text }) => (
          <EditCard
            key={name}
            title={name.replace(/^#+ /, "")}
            note={name in replace ? "Changed" : "Built-in"}
            text={replace[name] ?? text}
            online={online}
            save={(next) =>
              write({ ...replace, [name]: next }, mine.append ?? "")
            }
            reset={
              name in replace
                ? () => write({ ...replace, [name]: text }, mine.append ?? "")
                : undefined
            }
          />
        ))}
        <EditCard
          title="After the last section"
          note={mine.append ? "Changed" : ""}
          text={mine.append ?? ""}
          online={online}
          save={(next) => write(replace, next)}
        />
      </div>
    </details>
  );
}

// One text of the stack with its title: edited in place, saved or discarded.
function EditCard({
  title,
  note,
  text: shown,
  online,
  save,
  reset,
}: {
  title: string;
  // Beside the title: the file's path, or whether a section is changed.
  note: string;
  text: string;
  online: boolean;
  // Saves the text over what the edit started from; false when not saved yet.
  save: (text: string, base: string) => Promise<boolean>;
  reset?: () => Promise<boolean>;
}) {
  // Null until edited: the field shows the text as it is, including a newer
  // version loaded meanwhile, and an edit in progress is never replaced. An
  // edit remembers the text it started from, so saving over a file changed
  // meanwhile (another tab, the agent) is refused rather than lost.
  const [draft, setDraft] = useState<{ text: string; base: string } | null>(
    null,
  );
  const { run, busy, error } = useAction();
  const field = useRef<HTMLTextAreaElement>(null);
  const text = draft?.text ?? shown;
  const changed = text !== shown;
  // Leaves with the edit; focus stays in the field, not the page.
  const act = async (action: () => Promise<boolean>) => {
    await run(async () => {
      if (await action()) setDraft(null);
    });
    field.current?.focus();
  };
  return (
    <section class="instruction-card">
      <header>
        <strong>{title}</strong>
        <small class="muted" title={note}>
          <span dir="ltr">{note}</span>
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
            base: draft?.base ?? shown,
          })
        }
      />
      {(changed || reset) && (
        <Actions>
          {reset && !changed && (
            <Button disabled={busy || !online} onClick={() => act(reset)}>
              Reset
            </Button>
          )}
          {changed && (
            <>
              <Button
                disabled={busy}
                onClick={() => {
                  setDraft(null);
                  field.current?.focus();
                }}
              >
                Discard
              </Button>
              <Button
                variant="primary"
                disabled={busy || !online}
                onClick={() => act(() => save(text, draft?.base ?? shown))}
              >
                Save
              </Button>
            </>
          )}
        </Actions>
      )}
    </section>
  );
}
