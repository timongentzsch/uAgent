import { Field, Skeleton } from "./ui.tsx";

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
