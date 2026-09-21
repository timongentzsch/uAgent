import { Field, Skeleton } from "./ui.tsx";

const busy = { role: "status", "aria-busy": true } as const;

// Reserve the loaded surface's shell and controls. Unknown content stays
// generic; loading.tsx imports only ui.tsx so cold fallbacks stay cheap.
function Control() {
  return <Skeleton decorative rows={1} className="control-skeleton" />;
}
function Bars({ count = 3 }: { count?: number }) {
  return <Skeleton decorative rows={count} />;
}
function Actions() {
  return (
    <div className="dialog-actions" aria-hidden="true">
      <Skeleton decorative rows={1} className="action-skeleton" />
      <Skeleton decorative rows={1} className="action-skeleton" />
    </div>
  );
}

export function ModelSkeleton() {
  // Both callers own the form wrapper and actions. Reserve the optional
  // variant slot until the catalogue tells us whether it is available.
  return (
    <>
      <span className="sr-only" role="status" aria-busy="true">
        Loading models…
      </span>
      <Field label="Model · loading…">
        <Control />
      </Field>
      <div className="field-row" aria-hidden="true">
        <Field label="Effort">
          <Control />
        </Field>
        <Field label="Variant">
          <Control />
        </Field>
      </div>
    </>
  );
}

export function StatsSkeleton({
  turn = false,
  controls = false,
}: {
  turn?: boolean;
  controls?: boolean;
}) {
  return (
    <div {...busy} aria-label="Loading statistics…">
      {controls && (
        <div className="dialog-actions" aria-hidden="true">
          <button disabled>Turn</button>
          <button disabled>Session</button>
        </div>
      )}
      <dl className="stats">
        {Array.from({ length: turn ? 13 : 14 }, (_, i) => (
          <div key={i} aria-hidden="true">
            <dt>
              <Skeleton decorative rows={1} />
            </dt>
            <dd>
              <Skeleton decorative rows={1} />
            </dd>
          </div>
        ))}
      </dl>
    </div>
  );
}

export function SettingsSkeleton() {
  // Mirror the live settings form row for row: same Fields (labels,
  // values, help), real inert controls where the shape is textual
  // (disabled buttons, closed sections, disabled ranges render their
  // exact heights by construction), shimmer only for the selects.
  // The compact-surfaces spec pins loading-vs-loaded height parity.
  return (
    <div className="settings-content" {...busy} aria-label="Loading settings…">
      <div className="settings-fields" aria-hidden="true">
        <Field label="Appearance">
          <Control />
        </Field>
        <Field
          label="Zoom"
          value="100%"
          help="Scales the entire interface, conversation included."
        >
          <input type="range" disabled value={100} />
        </Field>
        <div className="dialog-actions">
          <button type="button" disabled>
            Reset zoom
          </button>
        </div>
        <Field
          label="Default permissions"
          help="Used by new conversations and conversations that inherit the default."
        >
          <Control />
        </Field>
        <button type="button" disabled>
          System prompt
        </button>
        <button type="button" disabled>
          Advanced configuration
        </button>
        <button type="button" disabled>
          UI showcase
        </button>
      </div>
      {["Install", "Notifications", "Paired devices"].map((name) => (
        <details key={name} className="settings-section" aria-hidden="true">
          <summary>{name}</summary>
        </details>
      ))}
    </div>
  );
}

export function RawBodySkeleton({
  decorative = false,
}: {
  decorative?: boolean;
}) {
  return (
    <div
      className="raw-body-loader"
      {...(decorative
        ? { "aria-hidden": true }
        : { ...busy, "aria-label": "Loading full body…" })}
    >
      <Skeleton decorative rows={10} className="code-skeleton" />
    </div>
  );
}

export function RawSkeleton({ http = false }: { http?: boolean }) {
  return (
    <div className="raw-content" {...busy} aria-label="Loading full body…">
      <div className="raw-controls" aria-hidden="true">
        {http && <Skeleton decorative rows={1} className="control-skeleton" />}
        <Skeleton decorative rows={1} className="control-skeleton" />
      </div>
      <div className="raw-body" aria-hidden="true">
        <RawBodySkeleton decorative />
      </div>
      <footer className="dialog-actions" aria-hidden="true">
        <Skeleton decorative rows={1} className="control-skeleton" />
        <Skeleton decorative rows={1} className="action-skeleton" />
      </footer>
    </div>
  );
}

export function LibraryRows({ count = 3 }: { count?: number }) {
  return (
    <div {...busy} aria-label="Loading library…">
      <ManagementRows count={count} />
    </div>
  );
}

function ManagementRows({ count = 3 }: { count?: number }) {
  return Array.from({ length: count }, (_, i) => (
    <div key={i} className="library-row" aria-hidden="true">
      <Skeleton decorative rows={1} className="title-skeleton" />
      <Skeleton decorative rows={1} className="meta-skeleton" />
    </div>
  ));
}

export function EditorSkeleton() {
  return (
    <div {...busy} aria-label="Loading document…">
      <div className="editor-head" aria-hidden="true">
        <Skeleton decorative rows={1} className="action-skeleton" />
        <Skeleton decorative rows={1} className="title-skeleton" />
      </div>
      <div className="field-row" aria-hidden="true">
        <Control />
        <Control />
      </div>
      <Bars count={5} />
      <Actions />
    </div>
  );
}

export function ManagementBodySkeleton({
  kind = "library",
}: {
  kind?: "library" | "scheduled";
}) {
  return (
    <div
      className="management-body"
      {...busy}
      aria-label={`Loading ${kind === "library" ? "library" : "scheduled tasks"}…`}
    >
      <div className="management-list" aria-hidden="true">
        {kind === "library" && (
          <>
            <Control />
            <Control />
          </>
        )}
        <ManagementRows count={kind === "library" ? 5 : 3} />
      </div>
      <div
        className={`management-editor ${kind === "scheduled" ? "schedule-editor" : ""}`}
        aria-hidden="true"
      >
        {kind === "scheduled" && (
          <div className="run-history">
            <h2>All runs</h2>
            <Bars count={2} />
          </div>
        )}
      </div>
    </div>
  );
}

export function ManagementSkeleton({
  kind = "library",
}: {
  kind?: "library" | "scheduled";
}) {
  return (
    <div className="management">
      <div className="management-toolbar" aria-hidden="true">
        {kind === "library" ? (
          <>
            <div className="segmented">
              <button disabled>Memories</button>
              <button disabled>Skills</button>
            </div>
            <Field label="Project">
              <Control />
            </Field>
            <button className="primary" disabled>
              Add memory
            </button>
          </>
        ) : (
          <>
            <p className="muted">
              Runs while the uAgent host is open. Your browser can close.
            </p>
            <button className="primary" disabled>
              New task
            </button>
          </>
        )}
      </div>
      <ManagementBodySkeleton kind={kind} />
    </div>
  );
}

export function PromptContentSkeleton() {
  return (
    <div
      className="prompt-content-skeleton"
      {...busy}
      aria-label="Loading system prompt…"
    >
      <Skeleton decorative rows={1} className="title-skeleton" />
      <div className="prompt-tabs segmented" aria-hidden="true">
        <button disabled>Instructions</button>
        <button disabled>Effective prompt</button>
      </div>
      <div className="prompt-columns effective" aria-hidden="true">
        <section>
          <h3>Instructions</h3>
          <Bars count={8} />
        </section>
        <section>
          <h3>Effective prompt</h3>
          <Bars count={8} />
        </section>
      </div>
    </div>
  );
}

export function PromptSkeleton() {
  return (
    <div className="prompt-editor">
      <div className="prompt-controls" aria-hidden="true">
        <Field label="Scope">
          <Control />
        </Field>
        <Field label="Project">
          <Control />
        </Field>
      </div>
      <PromptContentSkeleton />
    </div>
  );
}

export function ConversationActionSkeleton({ kind }: { kind: string }) {
  return (
    <div {...busy} aria-label="Loading…">
      {kind === "rename" ? <Control /> : <Bars count={3} />}
      <Actions />
    </div>
  );
}

export function DecisionSkeleton({ editor = false }: { editor?: boolean }) {
  return (
    <section className="decision" {...busy} aria-label="Loading decision…">
      <h2 aria-hidden="true">
        <Skeleton decorative rows={1} className="title-skeleton" />
      </h2>
      <div className="decision-preview" aria-hidden="true">
        <Bars count={3} />
      </div>
      {/* Real form element for identical shell styles; inert. */}
      <form aria-hidden="true" onSubmit={(event) => event.preventDefault()}>
        <label>
          {editor ? "System prompt" : "Response"}
          {editor ? <textarea rows={12} disabled /> : <Control />}
        </label>
        <Actions />
      </form>
    </section>
  );
}
