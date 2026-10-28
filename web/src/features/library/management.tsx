import type { CommandResults } from "../../shared/types.ts";
import { manage } from "../../state/api.ts";
import { Field, Select } from "../../shared/ui.tsx";
import { useResource } from "../../shared/use-resource.ts";
import "./management.css";

export function useLibrary(
  kind: "memory" | "skills",
  cwd: string,
  version: number,
  online: boolean,
) {
  const key = `${kind}:${cwd}`;
  // Another kind or folder shows nothing of the last one's list.
  const list = useResource<{ key: string; data: CommandResults["memory"] }>(
    (signal) =>
      cwd && online
        ? manage(kind, { cwd, action: "list" }, { signal }).then((data) => ({
            key,
            data,
          }))
        : undefined,
    [key, version, online],
  );
  return {
    data: list.value?.key === key ? list.value.data : null,
    error: list.error,
    refresh: list.retry,
  };
}
export function ScopeField({
  value,
  change,
}: {
  value: string;
  change: (value: string) => void;
}) {
  return (
    <Field label="Scope">
      <Select
        value={value}
        onChange={(event) => change(event.currentTarget.value)}
      >
        <option value="project">This folder</option>
        <option value="global">All conversations</option>
      </Select>
    </Field>
  );
}
