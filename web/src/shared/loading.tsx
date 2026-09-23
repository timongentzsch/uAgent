import { Field, Skeleton } from "./ui.tsx";

const busy = { role: "status", "aria-busy": true } as const;

// Placeholders only for known fields and text rows. Feature shells own their
// real controls; unknown content uses Spinner instead of copying a screen.
function Control() {
  return <Skeleton decorative rows={1} className="control-skeleton" />;
}
function ModelSkeleton() {
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

export function ModelLoading({ close }: { close: () => void }) {
  return (
    <div class="model-form">
      <ModelSkeleton />
      <ModelActions close={close} />
    </div>
  );
}

export function ModelActions({
  close,
  apply,
  disabled = false,
  busy = false,
}: {
  close: () => void;
  apply?: () => void;
  disabled?: boolean;
  busy?: boolean;
}) {
  return (
    <div class="dialog-actions">
      <button type="button" onClick={close}>
        Cancel
      </button>
      <button
        type="button"
        class="primary"
        disabled={!apply || disabled || busy}
        onClick={apply}
      >
        {busy ? "Applying…" : "Apply"}
      </button>
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
