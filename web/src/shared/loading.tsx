import { Field, Skeleton } from "./ui.tsx";

const busy = { role: "status", "aria-busy": true } as const;

// Structural mirrors, not generic bars. Each skeleton reuses the live
// shell's wrapper classes with inert shimmer blocks (never inputs,
// buttons or icons), so the layout that fades in already has the final
// shape. When a component changes shape, its mirror here changes too.
// loading.tsx imports only ui.tsx, so app-shell fallbacks stay cheap.
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
  // Both callers already own the `.model-form` wrapper and the real
  // Cancel/Apply actions. The mirror keeps the full Model + Effort +
  // Variant shape: variant-capable protocols (OpenRouter) always show
  // the Variant field, and the committed dialog specs pin both the
  // field count and loading-vs-loaded height parity on that shape.
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

export function StatsSkeleton() {
  return (
    <dl className="stats" {...busy} aria-label="Loading statistics…">
      {Array.from({ length: 8 }, (_, i) => (
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
  );
}

const historyShapes = ["user", "response", "tool", "response"] as const;
export function HistorySkeleton({ messages = 4 }: { messages?: number }) {
  return (
    <div {...busy} aria-label="Loading conversation…">
      {Array.from({ length: messages }, (_, i) => {
        const shape = historyShapes[i % historyShapes.length];
        return (
          <article key={i} className={`message ${shape}`} aria-hidden="true">
            {shape === "tool" ? (
              <div className="tool-row-head">
                <Skeleton decorative rows={1} className="control-skeleton" />
              </div>
            ) : (
              <>
                <header>
                  <Skeleton decorative rows={1} className="title-skeleton" />
                </header>
                <Bars count={shape === "user" ? 1 : 3} />
              </>
            )}
          </article>
        );
      })}
    </div>
  );
}

export function SidebarSkeleton() {
  return (
    <div {...busy} aria-label="Loading sessions…">
      <div className="sidebar-head" aria-hidden="true">
        <Skeleton decorative rows={1} className="brand-skeleton" />
        <Skeleton decorative rows={1} className="action-skeleton" />
      </div>
      <div className="sidebar-sections" aria-hidden="true">
        <Skeleton decorative rows={1} className="control-skeleton" />
        <Skeleton decorative rows={1} className="control-skeleton" />
      </div>
      <div className="search" aria-hidden="true">
        <Skeleton decorative rows={1} className="control-skeleton" />
      </div>
      <nav aria-hidden="true">
        {[0, 1].map((group) => (
          <section key={group}>
            <h2>
              <Skeleton decorative rows={1} className="title-skeleton" />
            </h2>
            {[0, 1, 2].map((row) => (
              <div key={row} className="session-row">
                <div className="session session-mirror">
                  <Skeleton decorative rows={1} className="title-skeleton" />
                  <Skeleton decorative rows={1} className="meta-skeleton" />
                </div>
              </div>
            ))}
          </section>
        ))}
      </nav>
    </div>
  );
}

export function ComposerSkeleton() {
  return (
    <section className="composer" {...busy} aria-label="Loading composer…">
      {/* Real form element for identical shell styles; inert. */}
      <form aria-hidden="true" onSubmit={(event) => event.preventDefault()}>
        <Skeleton decorative rows={1} className="composer-input-skeleton" />
        <div className="composer-actions">
          {[0, 1, 2].map((i) => (
            <Skeleton key={i} decorative rows={1} className="icon-skeleton" />
          ))}
          <Skeleton
            decorative
            rows={1}
            className="icon-skeleton send-skeleton"
          />
        </div>
      </form>
    </section>
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
      </div>
      {["Install", "Notifications", "Paired devices"].map((name) => (
        <details key={name} className="settings-section" aria-hidden="true">
          <summary>{name}</summary>
        </details>
      ))}
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
        <Skeleton decorative rows={10} className="code-skeleton" />
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
      {Array.from({ length: count }, (_, i) => (
        <div key={i} className="library-row" aria-hidden="true">
          <Skeleton decorative rows={1} className="title-skeleton" />
          <Skeleton decorative rows={1} className="meta-skeleton" />
        </div>
      ))}
    </div>
  );
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

export function ManagementSkeleton() {
  return (
    <div className="management" {...busy} aria-label="Loading workspace…">
      <div className="management-toolbar" aria-hidden="true">
        <Skeleton decorative rows={1} className="control-skeleton" />
        <Skeleton decorative rows={1} className="action-skeleton" />
      </div>
      <div className="management-list" aria-hidden="true">
        {[0, 1, 2, 3, 4].map((i) => (
          <div key={i} className="library-row">
            <Skeleton decorative rows={1} className="title-skeleton" />
            <Skeleton decorative rows={1} className="meta-skeleton" />
          </div>
        ))}
      </div>
    </div>
  );
}

export function PairingSkeleton() {
  return (
    <main className="pairing" {...busy} aria-label="Loading connection form…">
      <div aria-hidden="true">
        <Skeleton decorative rows={1} className="title-skeleton" />
        <Bars count={2} />
        <Control />
        <Actions />
      </div>
    </main>
  );
}

export function PromptSkeleton() {
  return (
    <div {...busy} aria-label="Loading system prompt…">
      <Bars count={2} />
      <Bars count={8} />
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

export function DecisionSkeleton() {
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
          Response
          <Control />
        </label>
        <Actions />
      </form>
    </section>
  );
}
