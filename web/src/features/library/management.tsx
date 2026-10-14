import type { CommandResults } from "../../shared/types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import { manage } from "../../state/api.ts";
import { Field, Select } from "../../shared/ui.tsx";
import "./management.css";

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
    const controller = new AbortController();
    setResult((value) =>
      value?.error ? { ...value, error: undefined } : value,
    );
    if (cwd && online)
      manage(kind, { cwd, action: "list" }, { signal: controller.signal })
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
      controller.abort();
    };
  }, [kind, cwd, version, attempt, online]);
  return {
    data: result?.key === key ? result.data || null : null,
    error: result?.key === key ? result.error : null,
    refresh: () => setAttempt((value) => value + 1),
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
        <option value="project">This project</option>
        <option value="global">All conversations</option>
      </Select>
    </Field>
  );
}
