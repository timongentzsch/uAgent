import { useEffect, useRef, useState } from "preact/hooks";
import type { ConfigChange, Configuration } from "../../../shared/types.ts";
import { manage } from "../../../state/api.ts";
import type { SavedScope } from "./facts.ts";

// What is saved at `scope` and the two ways to change it. The host answers:
// it holds what is saved for all conversations and, named a folder, for that
// project. It reads again whenever anyone saves (`version`). One instance
// serves one folder: its owner remounts it for another.
export function useConfiguration(
  scope: SavedScope,
  folder: string | undefined,
  version: number,
) {
  const where = folder ? { cwd: folder } : {};
  // Loads and saves answer in any order; only an answer to a request made
  // after the one last shown is shown.
  const asked = useRef(0);
  const shown = useRef(0);
  const [value, setValue] = useState<Configuration>();
  const show = (request: number, answer: Configuration) => {
    if (request < shown.current) return;
    shown.current = request;
    setValue(answer);
  };
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    const request = ++asked.current;
    setError(null);
    manage("config", { operation: "get", ...where }).then(
      (answer) => show(request, answer),
      (failure) => request >= shown.current && setError(failure),
    );
  }, [version, attempt]);
  // Saves in flight, and the setting the last failure belongs to.
  const [busy, setBusy] = useState(0);
  const [failed, setFailed] = useState<{ key: string; error: unknown }>();
  // Collected while settings are open, so several saves offer one restart.
  const [restart, setRestart] = useState<string[]>([]);
  const run = async (key: string, fields: Record<string, unknown>) => {
    setBusy((count) => count + 1);
    setFailed(undefined);
    const request = ++asked.current;
    try {
      const result = await manage("config", { scope, ...where, ...fields });
      show(request, result);
      const due = result.effects
        .filter((item) => item.effect === "restart")
        .map((item) => item.key);
      setRestart((current) => [...new Set([...current, ...due])]);
      return true;
    } catch (error) {
      setFailed({ key, error });
      return false;
    } finally {
      setBusy((count) => count - 1);
    }
  };
  return {
    settings: (value?.settings || []).filter((setting) =>
      setting.scopes.includes(scope),
    ),
    file: value?.file || "",
    document: value?.document,
    problem: value?.problem || "",
    categories: value?.categories || [],
    loaded: !!value,
    error,
    retry: () => setAttempt((prior) => prior + 1),
    busy: busy > 0,
    failed,
    restart,
    save: (change: ConfigChange) =>
      run(change.key, { operation: "apply", changes: [change] }),
    reset: () => run("", { operation: "reset" }),
  };
}
