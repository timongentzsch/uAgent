import type { SessionRef, State } from "./types.ts";
import { useState } from "preact/hooks";
import { useAction } from "./use-action.ts";
import { useResource } from "./use-resource.ts";
import { useApi } from "../state/api.ts";
import { Button, Field, Select, LoadError } from "./ui.tsx";
import { valueSources } from "./display.ts";
import { SearchSelect } from "./search-select.tsx";
import { ModelLoading, ModelActions } from "./loading.tsx";

export default function ModelPicker({
  session,
  state,
  online,
  running,
  close,
  selection,
  save,
  settings,
}: {
  session: SessionRef & { cwd?: string };
  state?: State;
  online: boolean;
  running: boolean;
  // With `save` the choice is handed back, not applied to a session: the
  // picker closes once it was taken.
  selection?: string;
  save?: (value: string, model: string, effort: string) => unknown;
  close: () => void;
  // Opens Settings at an address; a conversation's picker links its default.
  settings?: (address: string) => void;
}) {
  const { command } = useApi();
  const [model, setModel] = useState("");
  const [effort, setEffort] = useState(state?.effort || "default");
  const action = useAction();
  const {
    value: catalog,
    error: loadError,
    retry,
  } = useResource(
    async (signal) => {
      const response = save
        ? await command("models", null, { cwd: session.cwd })
        : await command("model", session, { operation: "catalog" });
      if (response.pending)
        throw new Error("Catalog is still loading. Try again shortly.");
      // A session opened meanwhile has its own choice.
      if (signal.aborted) return response.result;
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
      }
      return response.result;
    },
    [session.id, session.cwd],
  );
  // Where the model comes from, and the way to its default.
  const source = settings && (
    <div class="default-source">
      <small class="muted truncate">
        {valueSources[state?.route_source ?? ""]}
      </small>
      <Button
        variant="quiet"
        size="compact"
        onClick={() => {
          close();
          settings("models/model");
        }}
      >
        Change the default
      </Button>
    </div>
  );
  if (!catalog && !loadError)
    return <ModelLoading close={close} source={source} />;
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
  return (
    <div class="model-form">
      {!catalog ? (
        <LoadError error={loadError} retry={retry} />
      ) : (
        <>
          <SearchSelect
            label="Model"
            value={model}
            disabled={action.busy}
            options={[
              ...(model && !selected ? [{ value: model, label: model }] : []),
              ...catalog.models
                .map((item) => ({
                  value: item.value,
                  label: item.label,
                  // Listed under the provider that serves it.
                  group: item.label.split("/")[0],
                  detail: [
                    item.value !== item.label ? item.value : "",
                    item.name && item.name !== item.label ? item.name : "",
                    item.efforts?.length
                      ? `Supports ${item.efforts.join(", ")}`
                      : "",
                  ]
                    .filter(Boolean)
                    .join(" · "),
                }))
                .toSorted((a, b) => a.group.localeCompare(b.group)),
            ]}
            change={(value) => {
              setModel(value);
              setEffort("default");
            }}
          />
          {!!catalog.unavailable?.length && (
            <p class="muted small">
              No list from {catalog.unavailable.join(", ")}: check its address
              and key under{" "}
              {settings ? (
                <Button
                  variant="quiet"
                  size="compact"
                  onClick={() => {
                    close();
                    settings("models/providers");
                  }}
                >
                  Providers in Settings
                </Button>
              ) : (
                "Providers"
              )}
              .
            </p>
          )}
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
          </div>
          {source}
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
                [model, effort === "default" ? "" : effort]
                  .filter(Boolean)
                  .join(":"),
                model,
                effort,
              );
              if (taken !== false) return close();
              throw new Error("Not saved. Its row in Settings says why.");
            }
            const result = await command("model", session, {
              operation: "select",
              model,
              effort,
              // Nothing configured yet: the first choice is the default, as
              // the terminal's /model makes it.
              default: state?.route === "",
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
