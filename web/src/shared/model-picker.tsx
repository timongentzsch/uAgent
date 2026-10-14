import type { SessionRef, State, Model } from "./types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import { useAction } from "./use-action.ts";
import { useResource } from "./use-resource.ts";
import { command } from "../state/api.ts";
import { Field, Select, LoadError, Row, Switch } from "./ui.tsx";
import { ModelLoading, ModelActions } from "./loading.tsx";

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
  // With `save` the choice is handed back, not applied to a session: the
  // picker closes once it was taken.
  selection?: string;
  save?: (value: string) => unknown;
  close: () => void;
}) {
  const [model, setModel] = useState("");
  const [effort, setEffort] = useState(state?.effort || "default");
  const [variant, setVariant] = useState(state?.variant || "default");
  // The choice is this conversation's; saving it for all is asked for.
  // The first choice is for every conversation to come.
  const [forAll, setForAll] = useState(!save && state?.route === "");
  const action = useAction();
  const form = useRef<HTMLDivElement>(null);
  const {
    value: catalog,
    error: loadError,
    retry,
  } = useResource(
    async (live) => {
      const response = save
        ? await command("models", null, { cwd: session.cwd })
        : await command("model", session, { operation: "catalog" });
      if (response.pending)
        throw new Error("Catalog is still loading. Try again shortly.");
      // A session opened meanwhile has its own choice.
      if (!live()) return response.result;
      const chosen = response.result.models
        .filter(
          (item) =>
            selection === item.value || selection?.startsWith(`${item.value}:`),
        )
        .sort((a, b) => b.value.length - a.value.length)[0];
      // A saved selection the catalogue does not list is kept, not replaced.
      setModel(
        chosen?.value ||
          (save && selection) ||
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
      return response.result;
    },
    [session.id, session.cwd],
  );
  useEffect(() => {
    if (catalog)
      form.current?.querySelector("select")?.focus({ preventScroll: true });
  }, [catalog]);
  if (!catalog && !loadError) return <ModelLoading close={close} />;
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
        <LoadError error={loadError} retry={retry} />
      ) : (
        <>
          <Field label="Model">
            <Select
              aria-label="Model"
              value={model}
              disabled={action.busy}
              onChange={(event) => {
                setModel(event.currentTarget.value);
                setEffort("default");
                setVariant("default");
              }}
            >
              {model && !selected && <option value={model}>{model}</option>}
              {[...providers].map(([provider, models]) => (
                <optgroup key={provider} label={provider}>
                  {models.map((item) => (
                    <option value={item.value} key={item.value}>
                      {[
                        item.label,
                        item.name && item.name !== item.label ? item.name : "",
                        item.efforts?.length
                          ? `supports ${item.efforts.join(",")}`
                          : "",
                      ]
                        .filter(Boolean)
                        .join(" · ")}
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
                disabled={action.busy}
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
                  disabled={action.busy}
                  onChange={(event) => setVariant(event.currentTarget.value)}
                >
                  {variants.map((value) => (
                    <option key={value}>{value}</option>
                  ))}
                </Select>
              </Field>
            )}
          </div>
          {!save && (
            <Row
              label="Also use for new conversations"
              detail="Otherwise only this conversation changes."
            >
              <Switch
                label="Also use for new conversations"
                checked={forAll}
                disabled={action.busy}
                onChange={setForAll}
              />
            </Row>
          )}
          {loadError && <LoadError error={loadError} retry={retry} />}
          {action.error && <LoadError error={action.error} />}
        </>
      )}
      <ModelActions
        close={close}
        busy={action.busy}
        disabled={!catalog || !model || !online || running}
        apply={() =>
          action.run(async () => {
            if (save) {
              const taken = await save(
                [
                  model,
                  variant === "default" ? "" : variant,
                  effort === "default" ? "" : effort,
                ]
                  .filter(Boolean)
                  .join(":"),
              );
              if (taken !== false) return close();
              throw new Error("Not saved. Its row in Settings says why.");
            }
            const result = await command("model", session, {
              operation: "select",
              model,
              effort,
              variant,
              default: forAll,
            });
            if (result.pending)
              throw new Error(
                "Model change is still pending. Check the current model before retrying.",
              );
            close();
          })
        }
      />
    </div>
  );
}
