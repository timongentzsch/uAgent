import { useState } from "preact/hooks";
import type { ConfigChange, Configuration } from "../../../shared/types.ts";
import { useResource } from "../../../shared/use-resource.ts";
import { manage } from "../../../state/api.ts";
import type { SavedScope } from "./facts.ts";

// What is saved at `scope` and the two ways to change it. The host answers:
// it holds what is saved for all conversations and, named a folder, for that
// project. It reads again whenever anyone saves (`version`).
export function useConfiguration(
  scope: SavedScope,
  folder: string | undefined,
  version: number,
) {
  const where = folder ? { cwd: folder } : {};
  const config = useResource<Configuration>(
    () => manage("config", { operation: "get", ...where }),
    [folder, version],
  );
  // Saves in flight, and the setting the last failure belongs to.
  const [busy, setBusy] = useState(0);
  const [failed, setFailed] = useState<{ key: string; error: unknown }>();
  // Collected while settings are open, so several saves offer one restart.
  const [restart, setRestart] = useState<string[]>([]);
  const run = async (key: string, fields: Record<string, unknown>) => {
    setBusy((count) => count + 1);
    setFailed(undefined);
    try {
      const result = await manage("config", { scope, ...where, ...fields });
      config.setValue(result);
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
    settings: (config.value?.settings || []).filter(
      (setting) => !setting.terminal && setting.scopes.includes(scope),
    ),
    categories: config.value?.categories || [],
    loaded: !!config.value,
    error: config.error,
    retry: config.retry,
    busy: busy > 0,
    failed,
    restart,
    save: (change: ConfigChange) =>
      run(change.key, { operation: "apply", changes: [change] }),
    reset: () => run("", { operation: "reset" }),
  };
}
