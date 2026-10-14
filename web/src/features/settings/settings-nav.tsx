import { Group, Row } from "../../shared/ui.tsx";
import { SCOPES, type Scope } from "../../shared/settings-scopes.ts";

// Settings and its loading state draw the same list, so it never changes
// while the code arrives. A project is listed while a conversation is open.
export function SettingsNav({
  current,
  select,
  project,
}: {
  current?: Scope;
  select: (id: Scope) => void;
  // The open conversation's folder, by its last name.
  project?: string;
}) {
  return (
    <nav class="settings-nav" aria-label="Settings scopes">
      <Group>
        {SCOPES.map(
          ([id, label, Icon]) =>
            (id !== "project" || project) && (
              <Row
                key={id}
                label={
                  <span class="settings-nav-label">
                    <Icon />
                    {label}
                  </span>
                }
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
