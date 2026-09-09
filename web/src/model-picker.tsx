import type { SessionRef, State, ModelCatalogue, Model } from "./types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import { command } from "./store.ts";
import { Field, Select, LoadError } from "./ui.tsx";
import { ModelSkeleton } from "./loading.tsx";

export default function ModelPicker({
  session,
  state,
  online,
  running,
  close,
  selection,
  save,
}: {
  session: SessionRef & { cwd?: string };
  state?: State;
  online: boolean;
  running: boolean;
  selection?: string;
  save?: (value: string) => void;
  close: () => void;
}) {
  const [catalog, setCatalog] = useState<ModelCatalogue | null>(null);
  const [model, setModel] = useState("");
  const [effort, setEffort] = useState(state?.effort || "default");
  const [variant, setVariant] = useState(state?.variant || "default");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const generation = useRef(0);
  const form = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (catalog)
      form.current?.querySelector("select")?.focus({ preventScroll: true });
  }, [catalog]);
  async function loadCatalog() {
    const version = ++generation.current;
    setError(null);
    try {
      const response = save
        ? await command("models", null, { cwd: session.cwd })
        : await command("model", session, { operation: "catalog" });
      if (version !== generation.current) return;
      if (response.pending)
        throw new Error("Catalog is still loading. Try again shortly.");
      setCatalog(response.result);
      const chosen = response.result.models
        .filter(
          (item) =>
            selection === item.value || selection?.startsWith(`${item.value}:`),
        )
        .sort((a, b) => b.value.length - a.value.length)[0];
      setModel(
        chosen?.value ||
          response.result.models.find((item) => item.active)?.value ||
          response.result.models[0]?.value ||
          "",
      );
      if (chosen && selection) {
        const suffixes = selection.slice(chosen.value.length + 1).split(":");
        setEffort(
          suffixes.find((part) => chosen.efforts?.includes(part)) || "default",
        );
        setVariant(
          suffixes.find((part) => chosen.variants?.includes(part)) || "default",
        );
      }
    } catch (failure) {
      if (version === generation.current) setError(failure);
    }
  }
  useEffect(() => {
    loadCatalog();
    return () => {
      ++generation.current;
    };
  }, [session.id, session.cwd]);
  const providers = new Map<string, Model[]>();
  for (const item of catalog?.models || []) {
    const provider = item.label.includes("/")
      ? item.label.split("/")[0]
      : "Models";
    if (!providers.has(provider)) providers.set(provider, []);
    providers.get(provider)!.push(item);
  }
  const selected = catalog?.models.find((item) => item.value === model);
  const efforts = [
    ...new Set([
      "default",
      ...(selected?.efforts?.length
        ? selected.efforts
        : selected?.active
          ? state?.efforts || []
          : []),
    ]),
  ];
  const variants = [
    ...new Set(["default", ...(selected?.variants || state?.variants || [])]),
  ];
  return (
    <div class="model-form" ref={form}>
      {!catalog ? (
        error ? (
          <LoadError error={error} retry={loadCatalog} />
        ) : (
          <ModelSkeleton />
        )
      ) : (
        <>
          <Field label="Model">
            <Select
              aria-label="Model"
              value={model}
              disabled={busy}
              onChange={(event) => {
                setModel(event.currentTarget.value);
                setEffort("default");
                setVariant("default");
              }}
            >
              {[...providers].map(([provider, models]) => (
                <optgroup key={provider} label={provider}>
                  {models.map((item) => (
                    <option value={item.value} key={item.value}>
                      {item.label}
                    </option>
                  ))}
                </optgroup>
              ))}
            </Select>
          </Field>
          <div class="field-row">
            <Field label="Effort">
              <Select
                aria-label="Effort"
                value={effort}
                disabled={busy}
                onChange={(event) => setEffort(event.currentTarget.value)}
              >
                {efforts.map((value) => (
                  <option key={value}>{value}</option>
                ))}
              </Select>
            </Field>
            {variants.length > 1 && (
              <Field label="Variant">
                <Select
                  aria-label="Variant"
                  value={variant}
                  disabled={busy}
                  onChange={(event) => setVariant(event.currentTarget.value)}
                >
                  {variants.map((value) => (
                    <option key={value}>{value}</option>
                  ))}
                </Select>
              </Field>
            )}
          </div>
          {error && <LoadError error={error} />}
        </>
      )}
      <div class="dialog-actions">
        <button type="button" onClick={close}>
          Cancel
        </button>
        <button
          type="button"
          class="primary"
          disabled={busy || !catalog || !model || !online || running}
          onClick={async () => {
            setBusy(true);
            setError(null);
            try {
              if (save) {
                save(
                  [
                    model,
                    variant === "default" ? "" : variant,
                    effort === "default" ? "" : effort,
                  ]
                    .filter(Boolean)
                    .join(":"),
                );
                close();
                return;
              }
              const result = await command("model", session, {
                operation: "select",
                model,
                effort,
                variant,
              });
              if (result.pending)
                throw new Error(
                  "Model change is still pending. Check the current model before retrying.",
                );
              close();
            } catch (failure) {
              setError(failure);
            } finally {
              setBusy(false);
            }
          }}
        >
          {busy ? "Applying…" : "Apply"}
        </button>
      </div>
    </div>
  );
}
