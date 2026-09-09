import { Field, Mark, Skeleton } from "./ui.tsx";
import "./raw.css";
import "./statistics.css";
import "./settings.css";

const busy = { role: "status", "aria-busy": true } as const;

// The same field/row wrappers as the loaded surfaces; no second layout system.
function Control() {
  return <Skeleton decorative rows={1} className="control-skeleton" />;
}
export function ModelSkeleton() {
  return (
    <div class="model-form" {...busy} aria-label="Loading models…">
      <Field label="Model · loading…">
        <Control />
      </Field>
      <div class="field-row">
        <Field label="Effort">
          <Control />
        </Field>
        <Field label="Variant">
          <Control />
        </Field>
      </div>
    </div>
  );
}
export function StatsSkeleton() {
  return (
    <dl class="stats" {...busy} aria-label="Loading statistics…">
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
export function SettingsSkeleton() {
  return (
    <div class="settings-content" {...busy} aria-label="Loading settings…">
      <div class="settings-fields">
        <Field label="Appearance · loading…">
          <Control />
        </Field>
        {["Display size", "Text size"].map((label) => (
          <Field key={label} label={label}>
            <input type="range" disabled aria-hidden="true" />
          </Field>
        ))}
        <div class="dialog-actions">
          <button disabled>Reset sizes</button>
        </div>
        <Field
          label="Default permissions"
          help="Used by new conversations and conversations that inherit the default."
        >
          <Control />
        </Field>
        <button disabled>System prompt</button>
        <button disabled>Advanced configuration</button>
      </div>
      {["Install", "Notifications", "Paired devices"].map((label) => (
        <details key={label} class="settings-section" inert>
          <summary>{label}</summary>
        </details>
      ))}
    </div>
  );
}
export function RawSkeleton({ http = false }: { http?: boolean }) {
  return (
    <div class="raw-content" {...busy} aria-label="Loading full body…">
      <div class="raw-controls">
        <small class="loading-label">Loading full body…</small>
        {http && (
          <div class="raw-tabs">
            <button disabled>Request</button>
            <button disabled>Response</button>
          </div>
        )}
      </div>
      <div class="raw-body">
        <Skeleton decorative rows={12} />
      </div>
      <footer class="dialog-actions">
        <div class="raw-tabs" aria-hidden="true">
          <button disabled>Readable</button>
          <button disabled>Source</button>
        </div>
        <button disabled>Copy raw body</button>
        <button disabled>Download</button>
      </footer>
    </div>
  );
}
export function ComposerSkeleton() {
  return (
    <section class="composer" {...busy} aria-label="Loading composer…">
      <form>
        <textarea
          rows={1}
          placeholder="Loading composer…"
          disabled
          aria-hidden="true"
        />
        <div class="composer-actions">
          <span class="icon-button">
            <Control />
          </span>
          <div class="model-control">
            <Control />
          </div>
          <span class="icon-button">
            <Control />
          </span>
          <span class="icon-button">
            <Control />
          </span>
        </div>
      </form>
      <div class="status-line">
        <Skeleton decorative rows={1} />
      </div>
    </section>
  );
}
export function SidebarSkeleton() {
  return (
    <aside class="sidebar" {...busy} aria-label="Connecting to your host…">
      <div class="sidebar-head">
        <Mark />
        <button disabled>New conversation</button>
      </div>
      <div class="sidebar-sections">
        <button disabled>Library</button>
        <button disabled>Scheduled</button>
      </div>
      <div class="search">
        <Control />
      </div>
      <nav>
        <Skeleton decorative rows={1} />
        {Array.from({ length: 6 }, (_, i) => (
          <div key={i} class="session-row">
            <div class="session">
              <Skeleton decorative rows={2} />
            </div>
          </div>
        ))}
      </nav>
      <footer>Connecting…</footer>
    </aside>
  );
}

export function ManagementPageSkeleton({ compact }: { compact: boolean }) {
  return (
    <div
      style={{
        display: "flex",
        flexDirection: "column",
        flex: 1,
        minHeight: 0,
      }}
      {...busy}
      aria-label="Loading workspace controls…"
    >
      <div
        style={{ padding: "var(--pad)", borderBottom: "1px solid var(--line)" }}
      >
        <Skeleton decorative rows={2} />
      </div>
      <div style={{ display: "flex", flex: 1, minHeight: 0 }}>
        {!compact && (
          <div
            style={{
              flex: "0 0 15rem",
              padding: "var(--gap)",
              borderRight: "1px solid var(--line)",
            }}
          >
            <Skeleton decorative rows={8} />
          </div>
        )}
        <div style={{ flex: 1, padding: "var(--pad)" }}>
          <Skeleton decorative rows={12} />
        </div>
      </div>
    </div>
  );
}
