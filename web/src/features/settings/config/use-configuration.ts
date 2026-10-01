import { useState } from "preact/hooks";
import type {
  ConfigChange,
  Configuration,
  Session,
} from "../../../shared/types.ts";
import { useAction } from "../../../shared/use-action.ts";
import { useResource } from "../../../shared/use-resource.ts";
import { manage } from "../../../state/api.ts";

// The host's settings and the two ways to change them. A conversation
// answers for its own folder when it is idle; otherwise the host does.
export function useConfiguration(session?: Session) {
  const target = session?.generation && !session.turn_active ? session : null;
  const config = useResource<Configuration>(
    () => manage("config", { operation: "get" }, { session: target }),
    [target?.id],
  );
  const action = useAction();
  // Collected while settings are open, so several saves offer one restart.
  const [restart, setRestart] = useState<string[]>([]);
  const [shadowed, setShadowed] = useState<string[]>([]);
  const run = (fields: Record<string, unknown>) =>
    action.run(async () => {
      const result = await manage(
        "config",
        { scope: "user", ...fields },
        { session: target },
      );
      config.setValue(result);
      const keys = (effect: string) =>
        result.effects
          .filter((item) => item.effect === effect)
          .map((item) => item.key);
      setRestart((current) => [...new Set([...current, ...keys("restart")])]);
      setShadowed(keys("shadowed"));
    });
  return {
    settings: (config.value?.settings || []).filter(
      (setting) => !setting.terminal,
    ),
    loaded: !!config.value,
    error: config.error ?? action.error,
    retry: config.error != null ? config.retry : undefined,
    busy: action.busy,
    restart,
    shadowed,
    save: (change: ConfigChange) =>
      run({ operation: "apply", changes: [change] }),
    reset: () => run({ operation: "reset" }),
  };
}
