import { useState } from "preact/hooks";
import type {
  ConfigChange,
  Configuration,
  Session,
} from "../../../shared/types.ts";
import { useAction } from "../../../shared/use-action.ts";
import { useResource } from "../../../shared/use-resource.ts";
import { manage } from "../../../state/api.ts";
import type { SavedScope } from "./facts.ts";

// The settings and the two ways to change them at `scope`. The open
// conversation's runtime answers for its folder and itself (a stopped one
// starts for it); during its turn, or with none open, the host answers and
// knows only what is saved for all conversations.
export function useConfiguration(
  session?: Session,
  scope: SavedScope = "user",
) {
  const target = session && !session.turn_active ? session : null;
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
        { scope, ...fields },
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
      (setting) => !setting.terminal && setting.scopes.includes(scope),
    ),
    // A project's settings are its conversation's runtime's to answer.
    answered: scope === "user" || !!target,
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
