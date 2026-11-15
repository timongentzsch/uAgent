import { Group, Row } from "../../shared/ui.tsx";
import { SCOPES, type Scope } from "../../shared/settings-scopes.ts";

// Settings and its loading state draw the same list, so it never changes
// while the code arrives.
export function SettingsNav({
  current,
  select,
  project,
  class: className,
}: {
  current?: Scope;
  select: (id: Scope) => void;
  project?: string;
  class?: string;
}) {
  return (
    <nav class={`settings-nav ${className ?? ""}`} aria-label="Settings scopes">
      <Group>
        {SCOPES.map(
          ([id, label, Icon]) =>
            (id !== "project" || project) && (
              <Row
                key={id}
                icon={<Icon />}
                label={label}
                detail={id === "project" ? project : undefined}
                current={id === current}
                onClick={() => select(id)}
              />
            ),
        )}
      </Group>
    </nav>
  );
}
