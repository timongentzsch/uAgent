import type { CommandFields, CommandKind, CommandResults } from "./types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import { command } from "./store.ts";
import { Field, Select, Skeleton } from "./ui.tsx";
import "./management.css";

export async function manage<K extends CommandKind>(
  kind: K,
  fields: CommandFields = {},
): Promise<CommandResults[K]> {
  const result = await command(kind, null, fields);
  if (result.pending)
    throw new Error(
      "The operation is still pending. Refresh to inspect its result.",
    );
  return result.result;
}
export function useLibrary(
  kind: "memory" | "skills",
  cwd: string,
  version: number,
  online: boolean,
) {
  const key = `${kind}:${cwd}`;
  const [result, setResult] = useState<{
    key: string;
    data?: CommandResults["memory"];
    error?: unknown;
  }>();
  const [attempt, setAttempt] = useState(0);
  const generation = useRef(0);
  useEffect(() => {
    const current = ++generation.current;
    setResult((value) =>
      value?.error ? { ...value, error: undefined } : value,
    );
    if (cwd && online)
      manage(kind, { cwd, action: "list" })
        .then((value) => {
          if (generation.current === current) setResult({ key, data: value });
        })
        .catch((error) => {
          if (generation.current === current)
            setResult((value) => ({
              key,
              data: value?.key === key ? value.data : undefined,
              error,
            }));
        });
    return () => {
      ++generation.current;
    };
  }, [kind, cwd, version, attempt, online]);
  return {
    data: result?.key === key ? result.data || null : null,
    error: result?.key === key ? result.error : null,
    refresh: () => setAttempt((value) => value + 1),
  };
}
export function ProjectField({
  value,
  projects,
  change,
}: {
  value: string;
  projects: string[];
  change: (value: string) => void;
}) {
  return (
    <Field label="Project">
      <input
        list="management-projects"
        value={value}
        placeholder="Absolute directory on the host"
        onChange={(event) => change(event.currentTarget.value)}
      />
      <datalist id="management-projects">
        {projects.map((path) => (
          <option key={path} value={path} />
        ))}
      </datalist>
    </Field>
  );
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
        <option value="project">Project</option>
        <option value="global">Global</option>
      </Select>
    </Field>
  );
}
export function ManagementSkeleton() {
  return (
    <div
      class="management-body"
      role="status"
      aria-busy="true"
      aria-label="Loading scheduled tasks…"
    >
      <div class="management-list">
        <Skeleton rows={8} label="Loading tasks…" />
      </div>
      <div class="management-editor">
        <Skeleton rows={12} decorative />
      </div>
    </div>
  );
}
export const taskActive = (state: string) =>
  ["queued", "starting", "running", "waiting", "stopping"].includes(state);
export const dateTime = (seconds?: number, timezone?: string) =>
  seconds
    ? new Intl.DateTimeFormat(undefined, {
        dateStyle: "medium",
        timeStyle: "short",
        ...(timezone ? { timeZone: timezone } : {}),
      }).format(seconds * 1000)
    : "—";
