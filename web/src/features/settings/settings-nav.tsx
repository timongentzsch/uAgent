import { Folder, Monitor, SlidersHorizontal, Smartphone } from "lucide-preact";
import { Group, Row } from "../../shared/ui.tsx";

// What a setting affects is where it is found: the four scopes, each one
// row, with what it reaches said once over its page.
export const SCOPES = [
  [
    "user",
    "All conversations",
    SlidersHorizontal,
    () => "Every conversation, browser and terminal on this host.",
  ],
  [
    "project",
    "This project",
    Folder,
    (folder?: string) =>
      `Overrides All conversations for conversations in ${folder}.`,
  ],
  [
    "browser",
    "This browser",
    Monitor,
    () =>
      "Saved in this browser only. Other browsers and the terminal are not affected.",
  ],
  [
    "host",
    "Host",
    Smartphone,
    () => "This host and the browsers paired with it.",
  ],
] as const;
export type Scope = (typeof SCOPES)[number][0];

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
